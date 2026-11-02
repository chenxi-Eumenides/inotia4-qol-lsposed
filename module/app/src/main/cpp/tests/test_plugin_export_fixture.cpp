extern "C" __attribute__((visibility("default"))) int Fixture_Export_NotInAdapter(int value) {
    return value + 1;
}

extern "C" {
__attribute__((visibility("default"))) int Fixture_Data_NotFunction = 7;
}
