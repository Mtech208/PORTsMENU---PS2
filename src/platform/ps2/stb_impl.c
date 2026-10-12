// stb_impl.c - implementacion de stb_image para el menu PS2
// Carga PNG, JPG, BMP, TGA y GIF con un solo header (public domain).

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP

#include "stb_image.h"