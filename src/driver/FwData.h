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

/* Clave de un nombre ibt-<variant>[-<revision>[-<build>]].<ext>. */
struct FwKey {
    int variant;
    int revision;
    int build;
};

static inline bool fwKeyFromName(const char *name, FwKey *k) {
    k->variant = -1;
    k->revision = -1;
    k->build = -1;

    if (strncmp(name, "ibt-", 4) != 0)
        return false;

    const char *p = name + 4;
    int field = 0;
    while (*p && *p != '.') {
        if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            if (field == 0)
                k->variant = v;
            else if (field == 1)
                k->revision = v;
            else if (field == 2)
                k->build = v;
            field++;
        } else if (*p == '-') {
            p++;
        } else {
            /* ibt-hw-37.7... (BSEQ) u otro formato no soportado */
            return false;
        }
    }
    return field >= 1;
}

/*
 * Búsqueda tolerante SOLO dentro del mismo hw_variant.
 *
 * El bootloader puede pedir una revision algo distinta a la distribuida
 * (ibt-19-32-4.sfi frente a ibt-19-32-1.sfi), pero variant distinto es otro
 * chip: 0x11 JfP (AX200), 0x12 ThP, 0x13 HrP (AX201), 0x14 CcP. Servir
 * p.ej. un ibt-17-* a un AX201 hace que la descarga acabe en "Intel boot
 * failed" y el controlador se queda sin firmware (bucle de arranque).
 *
 * Regla: misma variante + misma extension; se puntua mas alto la revision
 * coincidente y, a igualdad, el build mas alto.
 */
static inline const FwDesc *findFwBestMatch(const char *name) {
    const FwDesc *exact = findFwByName(name);
    if (exact)
        return exact;

    FwKey want;
    if (!fwKeyFromName(name, &want) || want.variant < 0)
        return NULL;

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

    const FwDesc *best = NULL;
    int bestScore = -1;

    for (int i = 0; i < fwNumber; i++) {
        const char *n = fwList[i].name;
        size_t nl = strlen(n);
        if (nl < extLen)
            continue;
        if (memcmp(n + nl - extLen, name + len - extLen, extLen) != 0)
            continue;

        FwKey k;
        if (!fwKeyFromName(n, &k) || k.variant != want.variant)
            continue;

        int score = (k.revision == want.revision) ? 1000 : 0;
        if (k.build > 0)
            score += k.build;
        if (score > bestScore) {
            bestScore = score;
            best = &fwList[i];
        }
    }
    return best;
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
