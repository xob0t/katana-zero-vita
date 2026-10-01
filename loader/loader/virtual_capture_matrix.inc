/* Original CPU helper addresses/prologues. Not entry hooks. */
static const struct { const char *name; uint32_t offset, prologue[2]; } matrix_helpers[] = {
 {"_ZN9yyFrustum15FromViewProjMatERK8yyMatrix", 0xe9b078, {0xe92d4800,0xe1a0b00d}},
 {"_ZN14CCameraManager9GetCameraEi", 0xea1754, {0xe92d4c10,0xe28db008}},
 {"_ZN8yyMatrix8MultiplyEPS_S0_S0_", 0x12351a8, {0xed921a04,0xed915a01}},
 {"_ZN8yyMatrix8LookAtLHEPS_P9yyVector3S2_S2_", 0x1235570, {0xed913a01,0xeef73a00}},
 {"_ZN8yyMatrix7OrthoLHEPS_ffff", 0x1235a08, {0xee001a10,0xeeb50a40}},
};
