#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t Bitboard;

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };

/* Pieces are numbered color * 6 + type: P N B R Q K p n b r q k. */
enum { NO_PIECE = 12 };

/* Castling rights as bits. */
enum { WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8 };

/* Squares: a1 = 0, b1 = 1, ..., h8 = 63. */
enum {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    SQ_NONE
};

static inline int make_piece(int color, int type) { return color * 6 + type; }
static inline int piece_color(int piece) { return piece / 6; }
static inline int piece_type(int piece) { return piece % 6; }

static inline int make_square(int file, int rank) { return rank * 8 + file; }
static inline int file_of(int sq) { return sq & 7; }
static inline int rank_of(int sq) { return sq >> 3; }

/*
 * A move in 16 bits: from (6) | to (6) | flags (4).
 * Flag bit 4 marks captures, bit 8 promotions; the low two bits of a
 * promotion select the piece (knight, bishop, rook, queen).
 */
typedef uint16_t Move;

enum {
    MF_QUIET = 0,
    MF_DOUBLE_PUSH = 1,
    MF_KING_CASTLE = 2,
    MF_QUEEN_CASTLE = 3,
    MF_CAPTURE = 4,
    MF_EP_CAPTURE = 5,
    MF_PROMO = 8,
};

static inline Move move_encode(int from, int to, int flags) {
    return (Move)(from | to << 6 | flags << 12);
}
static inline int move_from(Move m) { return m & 63; }
static inline int move_to(Move m) { return m >> 6 & 63; }
static inline int move_flags(Move m) { return m >> 12; }
static inline bool move_is_capture(Move m) { return move_flags(m) & MF_CAPTURE; }
static inline bool move_is_promo(Move m) { return move_flags(m) & MF_PROMO; }
static inline int move_promo_type(Move m) { return KNIGHT + (move_flags(m) & 3); }

#endif
