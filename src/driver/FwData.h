/** @file
  Copyright (c) 2020 zxystd. All rights reserved.
  SPDX-License-Identifier: GPL-3.0-only
**/

//
//  FwData.h
//  IntelBluetoothFirmware
//
//  Created by zxystd on 2019/12/22.
//  Copyright © 2019 zxystd. All rights reserved.
//

#ifndef FwData_h
#define FwData_h
#include <string.h>
#include <libkern/c++/OSData.h>
#include <libkern/zlib.h>
#include <zutil.h>

struct FwDesc {
    const char *name;
    const unsigned char *data;  // zlib
    const long int size;        // tamano comprimido
    const long int rawSize;     // tamano descomprimido
};

extern const struct FwDesc fwList[];
extern const int fwNumber;

static inline const FwDesc *findFwByName(const char *name) {
    for (int i = 0; i < fwNumber; i++) {
        if (strcmp(fwList[i].name, name) == 0)
            return &fwList[i];
    }
    return NULL;
}

/*
 * Búsqueda tolerante: si el nombre exacto no está, se recortan uno o dos
 * grupos numéricos del nombre y se busca por prefijo, quedándose con el
 * firmware de mayor tamaño (el más reciente) de esa familia.
 *
 * Motivo: el bootloader puede reportar una revisión distinta a la del
 * fichero distribuido (p. ej. ibt-17-16-0.sfi frente a ibt-17-16-1.sfi).
 * Con la búsqueda exacta el arranque en frío fallaba en silencio y el
 * controlador quedaba sin firmware.
 */
static inline const FwDesc *findFwBestMatch(const char *name) {
    const FwDesc *exact = findFwByName(name);
    if (exact)
        return exact;

    size_t len = strlen(name);
    size_t extLen = 0;
    for (size_t i = len; i > 0; i--) {
        if (name[i - 1] == '.') {
            extLen = len - (i - 1);
            break;
        }
    }
    if (!extLen || extLen >= len)
        return NULL;

    const size_t baseLen = len - extLen;

    for (int cut = 1; cut <= 2; cut++) {
        size_t p = baseLen;
        int dashes = 0;
        size_t prefixLen = 0;
        while (p > 0) {
            if (name[p - 1] == '-') {
                if (++dashes == cut) {
                    prefixLen = p;
                    break;
                }
            }
            p--;
        }
        if (!prefixLen)
            break;

        const FwDesc *best = NULL;
        for (int i = 0; i < fwNumber; i++) {
            const char *n = fwList[i].name;
            if (strncmp(n, name, prefixLen) != 0)
                continue;
            size_t nl = strlen(n);
            if (nl < extLen)
                continue;
            if (memcmp(n + nl - extLen, name + baseLen, extLen) != 0)
                continue;
            if (!best || fwList[i].rawSize > best->rawSize)
                best = &fwList[i];
        }
        if (best)
            return best;
    }
    return NULL;
}

static inline bool uncompressFirmware(unsigned char *dest, uint *destLen, unsigned char *source, uint sourceLen)
{
    z_stream stream;
    int err;
    
    stream.next_in = source;
    stream.avail_in = sourceLen;
    stream.next_out = dest;
    stream.avail_out = *destLen;
    stream.zalloc = zcalloc;
    stream.zfree = zcfree;
    err = inflateInit(&stream);
    if (err != Z_OK) {
        return false;
    }
    err = inflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        inflateEnd(&stream);
        return false;
    }
    *destLen = (uint)stream.total_out;

    err = inflateEnd(&stream);
    return err == Z_OK;
}

#endif /* FwData_h */
