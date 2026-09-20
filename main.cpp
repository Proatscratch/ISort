#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
typedef struct ISort {
    SDL_Surface *Image_1;
    SDL_Surface *Image_2;
} ISort;
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

#define GIF_FLAVOR_C

static inline Uint8 clamp_byte(float val) {
    if (val < 0.0f) return 0;
    if (val > 255.0f) return 255;
    return (Uint8)val;
}

Uint32* getPixel(SDL_Surface *surface, int x, int y) {
    int bpp = surface->format->BytesPerPixel;
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;
    return (Uint32 *)p;
}

void CommitSort(ISort *sort, SDL_Surface *surface) {
    sort->Image_1 = surface;
}

typedef struct {
    float mean;
    float stdev;
} ImageStats;

ImageStats CalculateLuminanceStats(SDL_Surface *surface) {
    ImageStats stats = {0.0f, 0.0f};
    if (!surface) return stats;

    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

    size_t total_pixels = (size_t)surface->w * surface->h;
    if (total_pixels == 0) {
        if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
        return stats;
    }

    double sum = 0.0;
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint32 *px = getPixel(surface, x, y);
            Uint8 r, g, b, a;
            SDL_GetRGBA(*px, surface->format, &r, &g, &b, &a);

            float lum = 0.2126f * (r / 255.0f) + 0.7152f * (g / 255.0f) + 0.0722f * (b / 255.0f);
            sum += lum;
        }
    }
    stats.mean = (float)(sum / total_pixels);

    double variance_sum = 0.0;
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint32 *px = getPixel(surface, x, y);
            Uint8 r, g, b, a;
            SDL_GetRGBA(*px, surface->format, &r, &g, &b, &a);

            float lum = 0.2126f * (r / 255.0f) + 0.7152f * (g / 255.0f) + 0.0722f * (b / 255.0f);
            float diff = lum - stats.mean;
            variance_sum += diff * diff;
        }
    }
    stats.stdev = sqrtf((float)(variance_sum / total_pixels));

    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);

    return stats;
}

float EvaluatePixelCost(ISort *sort, int x1, int y1, int x2, int y2, float alpha) {
    if (!sort->Image_1 || !sort->Image_2) return 1e9f;

    Uint32 *px1 = getPixel(sort->Image_1, x1, y1);
    Uint32 *px2 = getPixel(sort->Image_2, x2, y2);

    Uint8 r1, g1, b1, a1;
    Uint8 r2, g2, b2, a2;

    SDL_GetRGBA(*px1, sort->Image_1->format, &r1, &g1, &b1, &a1);
    SDL_GetRGBA(*px2, sort->Image_2->format, &r2, &g2, &b2, &a2);

    float dr = (r1 - r2) / 255.0f;
    float dg = (g1 - g2) / 255.0f;
    float db = (b1 - b2) / 255.0f;
    float color_cost = sqrtf(dr * dr + dg * dg + db * db);

    float dx = (float)(x1 - x2);
    float dy = (float)(y1 - y2);
    float spatial_cost = sqrtf(dx * dx + dy * dy);

    return color_cost + (alpha * spatial_cost);
}

float* CommitCost(ISort *sort, SDL_Surface *surface) {
    sort->Image_2 = surface;

    if (!sort->Image_1 || !sort->Image_2) return NULL;

    int w = sort->Image_2->w;
    int h = sort->Image_2->h;

    float *cost_map = (float *)malloc(w * h * sizeof(float));
    if (!cost_map) return NULL;

    if (SDL_MUSTLOCK(sort->Image_1)) SDL_LockSurface(sort->Image_1);
    if (SDL_MUSTLOCK(sort->Image_2)) SDL_UnlockSurface(sort->Image_2);

    float alpha = 0.001f; 

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            cost_map[y * w + x] = EvaluatePixelCost(sort, x, y, x, y, alpha);
        }
    }

    if (SDL_MUSTLOCK(sort->Image_1)) SDL_UnlockSurface(sort->Image_1);
    if (SDL_MUSTLOCK(sort->Image_2)) SDL_UnlockSurface(sort->Image_2);

    return cost_map;
}

typedef struct {
    Uint8 r, g, b, a;
} PixelRGB;

static inline float PairCost(PixelRGB src_color, PixelRGB target_color, int src_x, int src_y, int target_x, int target_y, float alpha) {
    float dr = (src_color.r - target_color.r) / 255.0f;
    float dg = (src_color.g - target_color.g) / 255.0f;
    float db = (src_color.b - target_color.b) / 255.0f;
    float color_cost = sqrtf(dr * dr + dg * dg + db * db);

    float dx = (float)(src_x - target_x);
    float dy = (float)(src_y - target_y);
    float spatial_cost = sqrtf(dx * dx + dy * dy);

    return color_cost + (alpha * spatial_cost);
}

void Push(ISort *sort, const char *output_gif_path) {
    if (!sort || !sort->Image_1 || !sort->Image_2) return;

    int w = sort->Image_2->w;
    int h = sort->Image_2->h;

    ImageStats stats1 = CalculateLuminanceStats(sort->Image_1);
    ImageStats stats2 = CalculateLuminanceStats(sort->Image_2);
    if (stats1.stdev < 0.0001f) stats1.stdev = 1.0f;

    size_t total_pixels = (size_t)(w * h);

    PixelRGB *img1_grid = (PixelRGB *)malloc(total_pixels * sizeof(PixelRGB));
    PixelRGB *img2_grid = (PixelRGB *)malloc(total_pixels * sizeof(PixelRGB));
    uint8_t *gif_frame_buf = (uint8_t *)malloc(w * h * 4 * sizeof(uint8_t));

    if (!img1_grid || !img2_grid || !gif_frame_buf) {
        free(img1_grid);
        free(img2_grid);
        free(gif_frame_buf);
        return;
    }

    if (SDL_MUSTLOCK(sort->Image_1)) SDL_LockSurface(sort->Image_1);
    if (SDL_MUSTLOCK(sort->Image_2)) SDL_LockSurface(sort->Image_2);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;

            Uint32 *px1 = getPixel(sort->Image_1, x, y);
            Uint8 r1, g1, b1, a1;
            SDL_GetRGBA(*px1, sort->Image_1->format, &r1, &g1, &b1, &a1);
            img1_grid[idx] = (PixelRGB){r1, g1, b1, a1};

            Uint32 *px2 = getPixel(sort->Image_2, x, y);
            Uint8 r2, g2, b2, a2;
            SDL_GetRGBA(*px2, sort->Image_2->format, &r2, &g2, &b2, &a2);
            img2_grid[idx] = (PixelRGB){r2, g2, b2, a2};
        }
    }

    if (SDL_MUSTLOCK(sort->Image_1)) SDL_UnlockSurface(sort->Image_1);
    if (SDL_MUSTLOCK(sort->Image_2)) SDL_UnlockSurface(sort->Image_2);

    for (size_t i = 0; i < total_pixels; i++) {
        float rf = img2_grid[i].r / 255.0f;
        float gf = img2_grid[i].g / 255.0f;
        float bf = img2_grid[i].b / 255.0f;

        float unshaded_r = (rf - 0) * 1 + 0;
        float unshaded_g = (gf - 0) * 1 + 0;
        float unshaded_b = (bf - 0) * 1 + 0;

        img2_grid[i].r = clamp_byte(unshaded_r * 255.0f);
        img2_grid[i].g = clamp_byte(unshaded_g * 255.0f);
        img2_grid[i].b = clamp_byte(unshaded_b * 255.0f);
    }

    // Initialize GIF Writer (Delay: 5 = 50ms per frame, 8-bit depth)
    GifWriter gif_writer;
    GifBegin(&gif_writer, output_gif_path, w, h, 5, 8, true);

    float alpha = 0.001f;
    int MAX_PASSES = 40;
    float MIN_COST_GAIN = 3.0f;

    printf("Executing Full-Resolution Multi-Pass Sorting (%dx%d, max %d passes)...\n", w, h, MAX_PASSES);

    for (int pass = 1; pass <= MAX_PASSES; pass++) {
        int radius = (int)(fmaxf(w, h) * SDL_clamp(1.0f - ((float)(pass - 1) / 100), 0.2, 1));
        if (radius < 1) radius = 1;

        int swaps_performed = 0;
        float total_gain_this_pass = 0.0f;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int curr_idx = y * w + x;
                PixelRGB p_curr = img2_grid[curr_idx];
                PixelRGB target_curr = img1_grid[curr_idx];

                int best_swap_x = x;
                int best_swap_y = y;
                float best_gain = 0.0f;

                int min_y = (y - radius < 0) ? 0 : y - radius;
                int max_y = (y + radius >= h) ? h - 1 : y + radius;
                int min_x = (x - radius < 0) ? 0 : x - radius;
                int max_x = (x + radius >= w) ? w - 1 : x + radius;

                int step = (radius > 4) ? radius / 4 : 1;

                for (int ny = min_y; ny <= max_y; ny += step) {
                    for (int nx = min_x; nx <= max_x; nx += step) {
                        if (nx == x && ny == y) continue;

                        int neighbor_idx = ny * w + nx;
                        PixelRGB p_neighbor = img2_grid[neighbor_idx];
                        PixelRGB target_neighbor = img1_grid[neighbor_idx];

                        float current_cost = PairCost(p_curr, target_curr, x, y, x, y, alpha) +
                                             PairCost(p_neighbor, target_neighbor, nx, ny, nx, ny, alpha);

                        float swapped_cost = PairCost(p_neighbor, target_curr, nx, ny, x, y, alpha) +
                                             PairCost(p_curr, target_neighbor, x, y, nx, ny, alpha);

                        float gain = current_cost - swapped_cost;
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_swap_x = nx;
                            best_swap_y = ny;
                        }
                    }
                }

                if (best_gain > 0.0f) {
                    int swap_idx = best_swap_y * w + best_swap_x;
                    PixelRGB tmp = img2_grid[curr_idx];
                    img2_grid[curr_idx] = img2_grid[swap_idx];
                    img2_grid[swap_idx] = tmp;
                    swaps_performed++;
                    total_gain_this_pass += best_gain;
                }
            }
        }

        // Write frame to memory buffer in RGBA format
        for (size_t i = 0; i < total_pixels; i++) {
            gif_frame_buf[i * 4 + 0] = img2_grid[i].r;
            gif_frame_buf[i * 4 + 1] = img2_grid[i].g;
            gif_frame_buf[i * 4 + 2] = img2_grid[i].b;
            gif_frame_buf[i * 4 + 3] = img2_grid[i].a;
        }

        // Add frame to GIF stream
        GifWriteFrame(&gif_writer, gif_frame_buf, w, h, 5, 8, true);
        SDL_Surface *temp_surf = SDL_CreateRGBSurfaceWithFormatFrom(
            gif_frame_buf, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32
        );
        if (temp_surf) {
            IMG_SavePNG(temp_surf, "pass.png");
            SDL_FreeSurface(temp_surf);
        }
        printf("Pass %d/%d completed (%d swaps, gain: %.2f) -> Exported frame\n",
               pass, MAX_PASSES, swaps_performed, total_gain_this_pass);

        if (swaps_performed == 0 || total_gain_this_pass < MIN_COST_GAIN) {
            printf("Cost gain too low or converged. Halting early at pass %d.\n", pass);
            break;
        }
    }

    GifEnd(&gif_writer);

    free(img1_grid);
    free(img2_grid);
    free(gif_frame_buf);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <path_to_image1> <path_to_image2> [output.gif]\n", argv[0]);
        return 1;
    }

    const char *gif_output = (argc >= 4) ? argv[3] : "transformation.gif";

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        printf("Failed to initialize SDL_image: %s\n", IMG_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Surface *img1_raw = IMG_Load(argv[1]);
    SDL_Surface *img2_raw = IMG_Load(argv[2]);

    if (!img1_raw || !img2_raw) {
        printf("Failed to load images: %s\n", IMG_GetError());
        if (img1_raw) SDL_FreeSurface(img1_raw);
        if (img2_raw) SDL_FreeSurface(img2_raw);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Surface *img1 = SDL_ConvertSurfaceFormat(img1_raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_Surface *img2 = SDL_ConvertSurfaceFormat(img2_raw, SDL_PIXELFORMAT_RGBA32, 0);

    SDL_FreeSurface(img1_raw);
    SDL_FreeSurface(img2_raw);

    if (!img1 || !img2) {
        printf("Failed to convert image pixel formats.\n");
        if (img1) SDL_FreeSurface(img1);
        if (img2) SDL_FreeSurface(img2);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    ISort sort = {0};
    CommitSort(&sort, img1);

    printf("Evaluating cost map with CommitCost...\n");
    float *cost_map = CommitCost(&sort, img2);
    if (cost_map) {
        printf("Cost map computed successfully.\n");
        free(cost_map);
    }

    Push(&sort, gif_output);

    SDL_FreeSurface(img1);
    SDL_FreeSurface(img2);
    IMG_Quit();
    SDL_Quit();

    return 0;
}
