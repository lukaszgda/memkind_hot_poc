
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define MATRIX_SIZE 512
#define OBJS_NUM 10
#define LOOP_LEN 100000
#define AGE_THRESHOLD 20

double s;
int i,j,k;

void naive_matrix_multiply(double *a, double *b, double *c) {
    for(i=0;i<MATRIX_SIZE;i++) {
        for(j=0;j<MATRIX_SIZE;j++) {
            a[i * MATRIX_SIZE + j]=(double)i*(double)j;
            b[i * MATRIX_SIZE + j]=(double)i/(double)(j+5);
        }
    }

    for(j=0;j<MATRIX_SIZE;j++) {
        for(i=0;i<MATRIX_SIZE;i++) {
            s=0;
            for(k=0;k<MATRIX_SIZE;k++) {
                s+=a[i * MATRIX_SIZE + k]*b[k * MATRIX_SIZE + j];
            }
            c[i * MATRIX_SIZE + j] = s;
        }
    }

    s = 0.0;
    for(i=0;i<MATRIX_SIZE;i++) {
        for(j=0;j<MATRIX_SIZE;j++) {
            s+=c[i * MATRIX_SIZE + j];
        }
    }

    return;
}


int main(int argc, char **argv) {

    int mat_size = sizeof(double) * MATRIX_SIZE * MATRIX_SIZE;
    double *a = malloc(mat_size);
    double *b = malloc(mat_size);

    double* objs[OBJS_NUM];
    int ages[OBJS_NUM] = {0};
    int it;

    // fill frequency array
    // objects with lower ID are more frequent
    // 0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 4 ....
    const int FREQ_ARRAY_LEN = (OBJS_NUM * OBJS_NUM + OBJS_NUM) / 2;
    int freq[FREQ_ARRAY_LEN];
    int aa = 0, bb = 0;
    for (it = 0; it < FREQ_ARRAY_LEN; it++) {
        freq[it] = aa;
        aa++;
        if (aa > bb) {
            bb++;
            aa = 0;
        }
    }

    // allocate objects with different sizes
    for (it = 0; it < OBJS_NUM; it++) {
        objs[it] = malloc(mat_size + it * sizeof(double));

        // print start - end address of object
        printf("malloc: %d, start %llx, end %llx\n", it, 
            (long long unsigned int)(&objs[it][0]),
            (long long unsigned int)(&objs[it][MATRIX_SIZE * MATRIX_SIZE - 1]));
        fflush(stdout);
    }

    int sel = 0;
	for (it = 0; it < LOOP_LEN; it++) {

        // select dest object
        sel++;
        sel = sel % FREQ_ARRAY_LEN;
        int obj_id = freq[sel];

	    naive_matrix_multiply(a, b, objs[obj_id]);

        // each object has an age - if it goes above AGE_THRESHOLD
        // object is reallocated
        ages[obj_id]++;
        if (ages[obj_id] > AGE_THRESHOLD) {
            free(objs[obj_id]);
            objs[obj_id] = malloc(mat_size + obj_id * sizeof(double));
            ages[obj_id] = 0;
        }

        printf("obj: %d, res: %f\n", obj_id, s);
        fflush(stdout);
	}
}
