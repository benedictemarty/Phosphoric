/**
 * @file netutil.h
 * @brief Utilitaires réseau — parsing d'adresses hôte:port.
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US1, Sprint 125) : fonction pure, sans état ni
 * dépendance au cœur de l'émulateur.
 */
#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Parse une spécification "host[:port]".
 *
 * Si un ':' est présent (et pas en tête), la partie gauche est l'hôte et la
 * partie droite le port ; sinon toute la chaîne est l'hôte et `def_port` est
 * utilisé. `host` est toujours terminé par NUL et tronqué à `host_sz`.
 *
 * @param spec      Chaîne d'entrée (ex. "127.0.0.1:6551" ou "localhost").
 * @param host      Buffer de sortie pour l'hôte.
 * @param host_sz   Taille du buffer host.
 * @param port      Sortie : port parsé (ou def_port).
 * @param def_port  Port par défaut si absent.
 */
void parse_host_port(const char* spec, char* host, size_t host_sz,
                     uint16_t* port, uint16_t def_port);

#endif /* NETUTIL_H */
