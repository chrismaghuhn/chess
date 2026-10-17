#ifndef SELFPLAY_H
#define SELFPLAY_H

/* `engine selfplay --out FILE [options]`: plays games and writes training data. */
int selfplay_main(int argc, char **argv);

/* `engine data-info FILE [samples]`: statistics and sample positions of a data file. */
int data_info_main(int argc, char **argv);

#endif
