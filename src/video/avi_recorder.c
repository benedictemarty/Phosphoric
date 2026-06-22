/**
 * @file avi_recorder.c
 * @brief Motion-JPEG AVI video recorder (see avi_recorder.h)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-22
 * @version 1.0.0
 *
 * AVI layout produced (video-only, single MJPG stream):
 *
 *   RIFF <size> 'AVI '
 *     LIST <192> 'hdrl'
 *       'avih' <56>  MainAVIHeader
 *       LIST <116> 'strl'
 *         'strh' <56>  AVIStreamHeader (vids/MJPG)
 *         'strf' <40>  BITMAPINFOHEADER (MJPG)
 *     LIST <size> 'movi'
 *       '00dc' <size> <jpeg> [pad to even]
 *       ...
 *     'idx1' <16*N> index entries
 *
 * RIFF size, avih dwTotalFrames, strh dwLength, the suggested buffer
 * sizes and the movi LIST size are back-patched in avi_recorder_close().
 */

#include "video/avi_recorder.h"

#include <stdlib.h>
#include <string.h>

/* JPEG encoding via stb_image_write. The implementation lives in a single
 * TU (src/video/stb_image_write_impl.c); here we only need the declaration. */
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb_image_write.h"

/* ── little-endian writers ─────────────────────────────────────────── */

static void put_u16(FILE* fp, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, fp);
}

static void put_u32(FILE* fp, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, fp);
}

static void put_fourcc(FILE* fp, const char* cc) {
    fwrite(cc, 1, 4, fp);
}

/* Back-patch a 32-bit LE value at an absolute file offset. */
static void patch_u32(FILE* fp, long pos, uint32_t v) {
    long cur = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    put_u32(fp, v);
    fseek(fp, cur, SEEK_SET);
}

/* ── JPEG encode callback (append into recorder scratch buffer) ──────── */

typedef struct {
    avi_recorder_t* rec;
    bool oom;
} jpeg_ctx_t;

static void jpeg_append(void* context, void* data, int size) {
    jpeg_ctx_t* ctx = (jpeg_ctx_t*)context;
    avi_recorder_t* rec = ctx->rec;
    size_t need = rec->jpeg_len + (size_t)size;
    if (need > rec->jpeg_cap) {
        size_t cap = rec->jpeg_cap ? rec->jpeg_cap : 65536;
        while (cap < need) cap *= 2;
        uint8_t* nb = (uint8_t*)realloc(rec->jpeg_buf, cap);
        if (!nb) { ctx->oom = true; return; }
        rec->jpeg_buf = nb;
        rec->jpeg_cap = cap;
    }
    memcpy(rec->jpeg_buf + rec->jpeg_len, data, (size_t)size);
    rec->jpeg_len += (size_t)size;
}

/* The few back-patch positions are derived deterministically from the
 * fixed-layout header (everything before the movi list has constant size),
 * so we recompute them in close() rather than storing them per-recorder. */
#define AVI_RIFF_SIZE_POS        4L
#define AVI_AVIH_TOTALFRAMES_POS (12L /*RIFF+AVI */ + 8L /*LIST+size*/ + 4L /*hdrl*/ \
                                  + 8L /*avih+size*/ + 16L /*usec,maxbps,pad,flags*/)
/* avih layout after its 8-byte chunk header:
 *   +0  dwMicroSecPerFrame
 *   +4  dwMaxBytesPerSec
 *   +8  dwPaddingGranularity
 *   +12 dwFlags
 *   +16 dwTotalFrames   <-- patch
 *   +20 dwInitialFrames
 *   +24 dwStreams
 *   +28 dwSuggestedBufferSize  <-- patch
 */
#define AVI_AVIH_SUGGBUF_POS  (AVI_AVIH_TOTALFRAMES_POS + 12L)

/* strh chunk starts after avih chunk (64 bytes) + strl LIST header (12). */
#define AVI_STRH_DATA_POS  (12L + 8L + 4L /* up to & incl 'hdrl' */ \
                            + 64L /* avih chunk */ \
                            + 12L /* LIST+size+'strl' */ \
                            + 8L  /* 'strh'+size */)
/* strh layout after 8-byte chunk header:
 *   +0  fccType   +4 fccHandler  +8 dwFlags  +12 wPriority/wLanguage
 *   +16 dwInitialFrames  +20 dwScale  +24 dwRate  +28 dwStart
 *   +32 dwLength  <-- patch       +36 dwSuggestedBufferSize <-- patch
 */
#define AVI_STRH_LENGTH_POS   (AVI_STRH_DATA_POS + 32L)
#define AVI_STRH_SUGGBUF_POS  (AVI_STRH_DATA_POS + 36L)

bool avi_recorder_open(avi_recorder_t* rec, const char* filename,
                       int width, int height, int fps, int quality) {
    if (!rec || !filename || width <= 0 || height <= 0 || fps <= 0) return false;

    memset(rec, 0, sizeof(*rec));
    rec->fp = fopen(filename, "wb");
    if (!rec->fp) return false;

    rec->width = width;
    rec->height = height;
    rec->fps = fps;
    rec->quality = (quality < 1) ? 1 : (quality > 100 ? 100 : quality);

    FILE* fp = rec->fp;
    uint32_t usec = (uint32_t)(1000000 / fps);

    /* RIFF header (size patched on close) */
    put_fourcc(fp, "RIFF");
    put_u32(fp, 0);                /* size placeholder */
    put_fourcc(fp, "AVI ");

    /* LIST 'hdrl' (fixed size 192) */
    put_fourcc(fp, "LIST");
    put_u32(fp, 192);
    put_fourcc(fp, "hdrl");

    /* avih (56 bytes) */
    put_fourcc(fp, "avih");
    put_u32(fp, 56);
    put_u32(fp, usec);             /* dwMicroSecPerFrame */
    put_u32(fp, 0);                /* dwMaxBytesPerSec */
    put_u32(fp, 0);                /* dwPaddingGranularity */
    put_u32(fp, 0x10);             /* dwFlags = AVIF_HASINDEX */
    put_u32(fp, 0);                /* dwTotalFrames (patched) */
    put_u32(fp, 0);                /* dwInitialFrames */
    put_u32(fp, 1);                /* dwStreams */
    put_u32(fp, 0);                /* dwSuggestedBufferSize (patched) */
    put_u32(fp, (uint32_t)width);  /* dwWidth */
    put_u32(fp, (uint32_t)height); /* dwHeight */
    put_u32(fp, 0);                /* dwReserved[0] */
    put_u32(fp, 0);                /* dwReserved[1] */
    put_u32(fp, 0);                /* dwReserved[2] */
    put_u32(fp, 0);                /* dwReserved[3] */

    /* LIST 'strl' (fixed size 116) */
    put_fourcc(fp, "LIST");
    put_u32(fp, 116);
    put_fourcc(fp, "strl");

    /* strh (56 bytes) */
    put_fourcc(fp, "strh");
    put_u32(fp, 56);
    put_fourcc(fp, "vids");        /* fccType */
    put_fourcc(fp, "MJPG");        /* fccHandler */
    put_u32(fp, 0);                /* dwFlags */
    put_u16(fp, 0);                /* wPriority */
    put_u16(fp, 0);                /* wLanguage */
    put_u32(fp, 0);                /* dwInitialFrames */
    put_u32(fp, 1);                /* dwScale */
    put_u32(fp, (uint32_t)fps);    /* dwRate */
    put_u32(fp, 0);                /* dwStart */
    put_u32(fp, 0);                /* dwLength (patched) */
    put_u32(fp, 0);                /* dwSuggestedBufferSize (patched) */
    put_u32(fp, 0xFFFFFFFFu);      /* dwQuality */
    put_u32(fp, 0);                /* dwSampleSize */
    put_u16(fp, 0);                /* rcFrame.left */
    put_u16(fp, 0);                /* rcFrame.top */
    put_u16(fp, (uint16_t)width);  /* rcFrame.right */
    put_u16(fp, (uint16_t)height); /* rcFrame.bottom */

    /* strf = BITMAPINFOHEADER (40 bytes) */
    put_fourcc(fp, "strf");
    put_u32(fp, 40);
    put_u32(fp, 40);               /* biSize */
    put_u32(fp, (uint32_t)width);  /* biWidth */
    put_u32(fp, (uint32_t)height); /* biHeight */
    put_u16(fp, 1);                /* biPlanes */
    put_u16(fp, 24);               /* biBitCount */
    put_fourcc(fp, "MJPG");        /* biCompression */
    put_u32(fp, (uint32_t)(width * height * 3)); /* biSizeImage */
    put_u32(fp, 0);                /* biXPelsPerMeter */
    put_u32(fp, 0);                /* biYPelsPerMeter */
    put_u32(fp, 0);                /* biClrUsed */
    put_u32(fp, 0);                /* biClrImportant */

    /* LIST 'movi' (size patched on close). Record the position of the
     * 'movi' fourcc: idx1 offsets and movi_size are measured from here. */
    put_fourcc(fp, "LIST");
    put_u32(fp, 0);                /* movi LIST size placeholder */
    rec->movi_pos = ftell(fp);     /* position of 'movi' fourcc */
    put_fourcc(fp, "movi");

    if (ferror(fp)) { fclose(fp); rec->fp = NULL; return false; }
    return true;
}

bool avi_recorder_add_frame(avi_recorder_t* rec, const uint8_t* rgb) {
    if (!rec || !rec->fp || !rgb) return false;
    FILE* fp = rec->fp;

    /* Encode the frame to JPEG into the reusable scratch buffer. */
    rec->jpeg_len = 0;
    jpeg_ctx_t ctx = { rec, false };
    if (!stbi_write_jpg_to_func(jpeg_append, &ctx, rec->width, rec->height,
                                3, rgb, rec->quality) || ctx.oom) {
        return false;
    }

    uint32_t jlen = (uint32_t)rec->jpeg_len;

    /* Record index entry (offset relative to 'movi' fourcc). */
    if (rec->index_len >= rec->index_cap) {
        uint32_t cap = rec->index_cap ? rec->index_cap * 2 : 1024;
        avi_index_entry_t* ni =
            (avi_index_entry_t*)realloc(rec->index, cap * sizeof(*ni));
        if (!ni) return false;
        rec->index = ni;
        rec->index_cap = cap;
    }
    long chunk_pos = ftell(fp);
    rec->index[rec->index_len].offset = (uint32_t)(chunk_pos - rec->movi_pos);
    rec->index[rec->index_len].size = jlen;
    rec->index_len++;

    /* Write '00dc' chunk: fourcc, size, payload, pad to even. */
    put_fourcc(fp, "00dc");
    put_u32(fp, jlen);
    fwrite(rec->jpeg_buf, 1, jlen, fp);
    if (jlen & 1) {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, fp);
    }

    if (jlen > rec->max_jpeg_size) rec->max_jpeg_size = jlen;
    rec->frame_count++;
    return !ferror(fp);
}

bool avi_recorder_close(avi_recorder_t* rec) {
    if (!rec || !rec->fp) {
        if (rec) { free(rec->index); free(rec->jpeg_buf);
                   memset(rec, 0, sizeof(*rec)); }
        return true;
    }
    FILE* fp = rec->fp;

    /* End of movi data (just after the last chunk + pad). */
    long movi_end = ftell(fp);
    uint32_t movi_size = (uint32_t)(movi_end - rec->movi_pos); /* incl 'movi' fourcc */

    /* idx1 index. */
    put_fourcc(fp, "idx1");
    put_u32(fp, rec->index_len * 16u);
    for (uint32_t i = 0; i < rec->index_len; i++) {
        put_fourcc(fp, "00dc");
        put_u32(fp, 0x10);                 /* AVIIF_KEYFRAME */
        put_u32(fp, rec->index[i].offset);
        put_u32(fp, rec->index[i].size);
    }

    long file_end = ftell(fp);

    /* Back-patch sizes. */
    patch_u32(fp, AVI_RIFF_SIZE_POS, (uint32_t)(file_end - 8));
    patch_u32(fp, rec->movi_pos - 4, movi_size);
    patch_u32(fp, AVI_AVIH_TOTALFRAMES_POS, rec->frame_count);
    patch_u32(fp, AVI_AVIH_SUGGBUF_POS, rec->max_jpeg_size);
    patch_u32(fp, AVI_STRH_LENGTH_POS, rec->frame_count);
    patch_u32(fp, AVI_STRH_SUGGBUF_POS, rec->max_jpeg_size);

    bool ok = !ferror(fp);
    fclose(fp);
    free(rec->index);
    free(rec->jpeg_buf);
    memset(rec, 0, sizeof(*rec));
    return ok;
}
