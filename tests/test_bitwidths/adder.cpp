// 不同位宽的加法器测试
#include "common.h"

TEST(BitWidthAdder, BW8_NoCarry) {
    adderTest(8, 0x12, 0x34, false, 0x46, false);
}
TEST(BitWidthAdder, BW8_WithCarry) {
    adderTest(8, 0xFF, 0x01, false, 0x00, true);
}
TEST(BitWidthAdder, BW8_Cin) {
    adderTest(8, 0xFE, 0x01, true, 0x00, true);
}
TEST(BitWidthAdder, BW16) {
    adderTest(16, 0xFFFF, 1, false, 0, true);
}
TEST(BitWidthAdder, BW32) {
    adderTest(32, 0xFFFFFFFFULL, 1, false, 0, true);
}
TEST(BitWidthAdder, BW63) {
    adderTest(63, (1ULL << 63) - 1, 1, false, 0, true);
}
TEST(BitWidthAdder, BW64_Overflow) {
    adderTest(64, 0xFFFFFFFFFFFFFFFFULL, 1, false, 0, true);
}
TEST(BitWidthAdder, BW64_Cin) {
    adderTest(64, 0xFFFFFFFFFFFFFFFEULL, 1, true, 0, true);
}
TEST(BitWidthAdder, BW64_Normal) {
    adderTest(64, 0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL, false, 0x123456789ABCDEF0ULL + 0x0FEDCBA987654321ULL,
              false);
}
