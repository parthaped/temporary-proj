# Makefile for ECE 434 Sp26 Project 2 (Group 12)
# Builds: datagen, project1 (fixed Project 1 Part 1),
#         project2_p1 (Problem 1 with signals),
#         project2_p2 (Problem 2 Part 1),
#         project2_p2_q3 (Problem 2 Q3 masking).

CC = gcc
CFLAGS = -Wall -Wextra -O2

BINS = datagen project1 project2_p1 project2_p2 project2_p2_q3

all: $(BINS)

datagen: datagen.c
	$(CC) $(CFLAGS) datagen.c -o datagen

project1: project1.c
	$(CC) $(CFLAGS) project1.c -o project1

project2_p1: project2_p1.c
	$(CC) $(CFLAGS) project2_p1.c -o project2_p1

# alternate build for the SIG_IGN experiment of RULE 3
project2_p1_exp2: project2_p1.c
	$(CC) $(CFLAGS) -DRULE3_EXPERIMENT=2 project2_p1.c -o project2_p1_exp2

project2_p2: project2_p2.c
	$(CC) $(CFLAGS) project2_p2.c -o project2_p2

project2_p2_q3: project2_p2_q3.c
	$(CC) $(CFLAGS) project2_p2_q3.c -o project2_p2_q3

clean:
	rm -f $(BINS) project2_p1_exp2 input.txt trace.csv output.txt

.PHONY: all clean
