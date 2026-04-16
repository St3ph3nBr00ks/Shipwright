#include "PronounSystem.h"
#include <stdint.h>
#include <string.h>

#include "libultraship/bridge/consolevariablebridge.h"

// ---------------------------------------------------------------------------
// Control-code argument counts for OoT message encoding (0x01–0x1F).
// Bytes in this range are control codes, not printable text. Each may be
// followed by N argument bytes that must be skipped together with the code.
// ---------------------------------------------------------------------------
static const uint8_t sCtrlArgCount[32] = {
    //  0x00  0x01  0x02  0x03  0x04  0x05  0x06  0x07
        0,    0,    0,    0,    0,    1,    1,    2,
    //  0x08  0x09  0x0A  0x0B  0x0C  0x0D  0x0E  0x0F
        0,    0,    0,    0,    1,    0,    1,    0,
    //  0x10  0x11  0x12  0x13  0x14  0x15  0x16  0x17
        0,    2,    2,    1,    1,    3,    0,    0,
    //  0x18  0x19  0x1A  0x1B  0x1C  0x1D  0x1E  0x1F
        0,    0,    0,    0,    0,    0,    1,    0,
};

static inline int CtrlArgCount(unsigned char c) {
    if (c < 32) return sCtrlArgCount[c];
    return 0;
}

static inline int IsAlpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// ---------------------------------------------------------------------------
// Pronoun replacement table.
// Ordered longest-first within each case group to avoid partial matches
// (e.g. "Himself" before "Him", "himself" before "him").
// ---------------------------------------------------------------------------
struct PronounPair {
    const char* from;
    int         flen;
    const char* to_she;   // mode 1: she/her/her
    int         tlen_she;
    const char* to_they;  // mode 2: they/them/their
    int         tlen_they;
};

static const PronounPair sPronounPairs[] = {
    { "Himself", 7, "Herself",  7, "Themself", 8 },
    { "himself", 7, "herself",  7, "themself", 8 },
    { "Him",     3, "Her",      3, "Them",     4 },
    { "him",     3, "her",      3, "them",     4 },
    { "His",     3, "Her",      3, "Their",    5 },
    { "his",     3, "her",      3, "their",    5 },
    { "He",      2, "She",      3, "They",     4 },
    { "he",      2, "she",      3, "they",     4 },
};
static const int sPronounPairCount = (int)(sizeof(sPronounPairs) / sizeof(sPronounPairs[0]));

// ---------------------------------------------------------------------------
// Single-pass in-place replacement.
// Uses a static scratch buffer so no heap allocation is needed at message time.
// msgBuf is always 1280 bytes (Font::msgBuf). We write into sTempBuf and copy
// back, so we never overflow the source buffer.
// ---------------------------------------------------------------------------
static char sTempBuf[1280];

extern "C" void PronounSystem_FilterMessageBuffer(char* buf, int* length) {
    int mode = CVarGetInteger(CVAR_PRONOUN_MODE, 0);
    if (mode <= 0) return;

    const int srcLen = *length;
    if (srcLen <= 0 || srcLen > 1280) return;

    int  ri  = 0;  // read index into buf
    int  wi  = 0;  // write index into sTempBuf
    bool prevWasAlpha = false;  // tracks last *text* character, not raw bytes

    while (ri < srcLen) {
        unsigned char c = (unsigned char)buf[ri];

        // --- Control code: skip code byte + argument bytes ---
        if (c >= 0x01 && c <= 0x1F) {
            int args = CtrlArgCount(c);
            // Copy the code and its arguments verbatim.
            // Do NOT update prevWasAlpha — control code arg bytes that happen
            // to be alpha ASCII values must not block a following pronoun match.
            for (int k = 0; k <= args && ri < srcLen; k++, ri++) {
                if (wi < 1280) sTempBuf[wi++] = buf[ri];
            }
            continue;
        }

        // --- Printable text character: try pronoun replacement ---
        if (!prevWasAlpha) {
            // Check each pronoun at this position
            bool replaced = false;
            for (int p = 0; p < sPronounPairCount; p++) {
                const PronounPair& pp = sPronounPairs[p];
                if (ri + pp.flen > srcLen) continue;
                if (strncmp(buf + ri, pp.from, pp.flen) != 0) continue;

                // Confirm right word boundary: next char is not alpha text.
                // If ri+flen is a control code, that is not alpha — OK.
                unsigned char nextC = (ri + pp.flen < srcLen)
                    ? (unsigned char)buf[ri + pp.flen] : 0;
                if (IsAlpha(nextC)) continue;  // "him" inside "himself" etc.

                const char* toStr = (mode == 1) ? pp.to_she  : pp.to_they;
                int         toLen = (mode == 1) ? pp.tlen_she : pp.tlen_they;

                if (wi + toLen > 1280) break;  // safety: abort this replacement
                memcpy(sTempBuf + wi, toStr, toLen);
                wi += toLen;
                ri += pp.flen;

                // The last char of the replacement is alpha
                prevWasAlpha = true;
                replaced = true;
                break;
            }
            if (replaced) continue;
        }

        // No replacement: copy verbatim and update prevWasAlpha
        if (wi < 1280) sTempBuf[wi++] = buf[ri];
        prevWasAlpha = IsAlpha(c);
        ri++;
    }

    // Copy result back
    if (wi <= 1280) {
        memcpy(buf, sTempBuf, wi);
        *length = wi;
    }
}
