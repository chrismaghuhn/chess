#ifndef EVAL_H
#define EVAL_H

#include "board.h"

/*
 * Score from the side to move's view. Generation 0 knows nothing about
 * chess beyond the rules, so every position scores 0 and only mates and
 * draws found by the search matter. Milestone 3 replaces this with the
 * learned linear evaluation.
 */
int eval_evaluate(const Board *b);

#endif
