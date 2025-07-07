#ifndef CONFIG_HPP
#define CONFIG_HPP

#define UPDATE_THREADS 16
#define UPLOAD_THREADS 128
#define FRAMES 2

#define WIDTH 960
#define HEIGHT 720

#define EMPTY 0u
#define STONE 1u
#define SAND 2u
#define WATER 3u

/* 4 particles per uint */
#define PARTICLE0 0
#define PARTICLE1 8
#define PARTICLE2 16
#define PARTICLE3 24

/* particles travelling different directions are written to different masks to DECREASE races */
#define PARTICLEU PARTICLE0
#define PARTICLED PARTICLE1
#define PARTICLEL PARTICLE2
#define PARTICLER PARTICLE3

#endif