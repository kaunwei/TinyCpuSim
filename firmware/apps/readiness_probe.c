__attribute__((noinline)) unsigned probe_select(unsigned x) {
    return x ? 7u : 9u;
}

__attribute__((noinline)) int probe_negate(int x) {
    return -x;
}

__attribute__((noinline)) unsigned probe_compare(int x) {
    return x == -128;
}

int main(void) {
    return 0;
}
