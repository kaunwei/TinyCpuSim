#include <stdint.h>

volatile int32_t C[4][4];
const int32_t A[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {1, 0, 1, 0}, {0, 1, 0, 1}};
const int32_t B[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

int main(void) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            int32_t sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += A[i][k] * B[k][j];
            }
            C[i][j] = sum;
        }
    }
    return C[0][0];
}
