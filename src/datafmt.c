#include "datafmt.h"

#include <stdlib.h>
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

void data_pack(const Board *b, int result, int ply, int score, bool score_valid, uint8_t out[DATA_RECORD_SIZE]) {
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
    int16_t s = (int16_t)(!score_valid ? 0 : score > 32767 ? 32767 : score < -32768 ? -32768 : score);
    put_le(out + 32, (uint16_t)s, 2);
    out[34] = score_valid ? 1 : 0;
}

void data_set_outcome(uint8_t record[DATA_RECORD_SIZE], int white_result, GameEnd end) {
    record[28] = (uint8_t)(record[24] == WHITE ? white_result : RESULT_WIN - white_result);
    record[29] = (uint8_t)end;
}

bool data_unpack(const uint8_t in[DATA_RECORD_SIZE], DataPosition *out) {
    Bitboard occupied = get_le(in, 8);
    int pieces = popcount(occupied);
    int16_t score = (int16_t)get_le(in + 32, 2);
    if (pieces > 32 || in[28] > RESULT_WIN || in[29] >= END_COUNT || in[34] > 1 || in[35]
        || (!in[34] && score))
        return false;
    for (int n = pieces; n < 32; n++)
        if (in[8 + n / 2] >> (4 * (n & 1)) & 15)
            return false; /* unused nibbles must be zero */

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
    out->end = (GameEnd)in[29];
    out->ply = ply;
    out->score = score;
    out->score_valid = in[34] == 1;
    return true;
}

bool data_read(FILE *f, int record_size, uint8_t out[DATA_RECORD_SIZE]) {
    memset(out, 0, DATA_RECORD_SIZE);
    return fread(out, 1, (size_t)record_size, f) == (size_t)record_size;
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

FILE *data_open(const char *path, uint64_t *count, int *record_size, char *err, size_t err_size) {
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
    uint64_t version = get_le(header + 4, 4), size_field = get_le(header + 8, 4);
    if (!((version == 1 && size_field == DATA_RECORD_SIZE_V1) || (version == 2 && size_field == DATA_RECORD_SIZE))
        || get_le(header + 12, 4) != 0) {
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
    if (size < DATA_HEADER_SIZE || (size - DATA_HEADER_SIZE) % (long)size_field != 0) {
        snprintf(err, err_size, "%s: truncated (size %ld)", path, size);
        fclose(f);
        return NULL;
    }
    fseek(f, DATA_HEADER_SIZE, SEEK_SET);
    *count = (uint64_t)(size - DATA_HEADER_SIZE) / size_field;
    *record_size = (int)size_field;
    return f;
}

Board *data_load_starts(const char *path, int max_pieces, size_t *count, char *err, size_t err_size) {
    uint64_t records;
    int record_size;
    FILE *f = data_open(path, &records, &record_size, err, err_size);
    if (!f)
        return NULL;
    Board *starts = malloc((records ? records : 1) * sizeof *starts);
    if (!starts) {
        fclose(f);
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    *count = 0;
    uint8_t r[DATA_RECORD_SIZE];
    for (uint64_t i = 0; i < records && data_read(f, record_size, r); i++) {
        DataPosition pos;
        if (data_unpack(r, &pos) && popcount(pos.board.all) <= max_pieces)
            starts[(*count)++] = pos.board;
    }
    fclose(f);
    if (!*count) {
        free(starts);
        snprintf(err, err_size, "%s has no position with at most %d pieces", path, max_pieces);
        return NULL;
    }
    return starts;
}
