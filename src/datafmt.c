#include "datafmt.h"

#include <string.h>

static const char data_magic[4] = { 'S', 'C', 'H', 'D' };

static void put_le(uint8_t *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

void data_pack(const Board *b, int result, int ply, uint8_t out[DATA_RECORD_SIZE]) {
    memset(out, 0, DATA_RECORD_SIZE);
    put_le(out, b->all, 8);

    int n = 0;
    for (Bitboard bb = b->all; bb; n++) {
        int piece = b->squares[pop_lsb(&bb)];
        out[8 + n / 2] |= (uint8_t)(piece << (4 * (n & 1)));
    }

    out[24] = b->side;
    out[25] = b->castling;
    out[26] = b->ep;
    out[27] = (uint8_t)(b->halfmove < 255 ? b->halfmove : 255);
    out[28] = (uint8_t)result;
    put_le(out + 30, (uint64_t)ply, 2);
}

void data_set_result(uint8_t record[DATA_RECORD_SIZE], int white_result) {
    record[28] = (uint8_t)(record[24] == WHITE ? white_result : RESULT_WIN - white_result);
}

bool data_unpack(const uint8_t in[DATA_RECORD_SIZE], DataPosition *out) {
    Bitboard occupied = get_le(in, 8);
    if (popcount(occupied) > 32 || in[28] > RESULT_WIN)
        return false;

    uint8_t squares[64];
    memset(squares, NO_PIECE, sizeof squares);
    int n = 0;
    for (Bitboard bb = occupied; bb; n++) {
        int piece = in[8 + n / 2] >> (4 * (n & 1)) & 15;
        if (piece >= NO_PIECE)
            return false;
        squares[pop_lsb(&bb)] = (uint8_t)piece;
    }

    int ply = (int)get_le(in + 30, 2);
    if (!board_setup(&out->board, squares, in[24], in[25], in[26], in[27], ply / 2 + 1))
        return false;
    out->result = in[28];
    out->ply = ply;
    return true;
}

FILE *data_create(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    uint8_t header[DATA_HEADER_SIZE] = { 0 };
    memcpy(header, data_magic, 4);
    put_le(header + 4, DATA_VERSION, 4);
    put_le(header + 8, DATA_RECORD_SIZE, 4);
    if (fwrite(header, 1, sizeof header, f) != sizeof header) {
        fclose(f);
        return NULL;
    }
    return f;
}

FILE *data_open(const char *path, uint64_t *count, char *err, size_t err_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "cannot open %s", path);
        return NULL;
    }

    uint8_t header[DATA_HEADER_SIZE];
    if (fread(header, 1, sizeof header, f) != sizeof header || memcmp(header, data_magic, 4) != 0) {
        snprintf(err, err_size, "%s is not a data file", path);
        fclose(f);
        return NULL;
    }
    if (get_le(header + 4, 4) != DATA_VERSION || get_le(header + 8, 4) != DATA_RECORD_SIZE) {
        snprintf(err, err_size, "%s: unsupported version or record size", path);
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, err_size, "%s: cannot seek", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < DATA_HEADER_SIZE || (size - DATA_HEADER_SIZE) % DATA_RECORD_SIZE != 0) {
        snprintf(err, err_size, "%s: truncated (size %ld)", path, size);
        fclose(f);
        return NULL;
    }
    fseek(f, DATA_HEADER_SIZE, SEEK_SET);
    *count = (uint64_t)(size - DATA_HEADER_SIZE) / DATA_RECORD_SIZE;
    return f;
}
