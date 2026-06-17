// 不同位宽的门测试：多输入门 + NOT/BUF
#include "common.h"

TEST(BitWidthGate, BW_1) {
    gateBitWidthTest(1);
}
TEST(BitWidthGate, BW_7) {
    gateBitWidthTest(7);
}
TEST(BitWidthGate, BW_8) {
    gateBitWidthTest(8);
}
TEST(BitWidthGate, BW_9) {
    gateBitWidthTest(9);
}
TEST(BitWidthGate, BW_15) {
    gateBitWidthTest(15);
}
TEST(BitWidthGate, BW_16) {
    gateBitWidthTest(16);
}
TEST(BitWidthGate, BW_17) {
    gateBitWidthTest(17);
}
TEST(BitWidthGate, BW_31) {
    gateBitWidthTest(31);
}
TEST(BitWidthGate, BW_32) {
    gateBitWidthTest(32);
}
TEST(BitWidthGate, BW_33) {
    gateBitWidthTest(33);
}
TEST(BitWidthGate, BW_63) {
    gateBitWidthTest(63);
}
TEST(BitWidthGate, BW_64) {
    gateBitWidthTest(64);
}

TEST(BitWidthNOT, BW_1) {
    notBitWidthTest(1);
}
TEST(BitWidthNOT, BW_7) {
    notBitWidthTest(7);
}
TEST(BitWidthNOT, BW_8) {
    notBitWidthTest(8);
}
TEST(BitWidthNOT, BW_16) {
    notBitWidthTest(16);
}
TEST(BitWidthNOT, BW_32) {
    notBitWidthTest(32);
}
TEST(BitWidthNOT, BW_64) {
    notBitWidthTest(64);
}
