#include <stdlib.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "8080.h"

#define DISPLAY_SCALE 2
#define WIDTH 224
#define HEIGHT 256

int game_running = false;

SDL_Window *window = NULL;
SDL_Surface *surface = NULL;

uint8_t shift0 = 0;
uint8_t shift1 = 0;
uint8_t shift_offset = 0;

uint8_t in_port_1 = 0;
uint8_t in_port_2 = 0;
uint8_t in_port_3 = 0;

uint8_t out_port_2 = 0;
uint8_t out_port_4= 0;

uint8_t sound1_ = 0;
uint8_t sound2_ = 0;
uint8_t last_sound1_ = 0;
uint8_t last_sound2_ = 0;

State8080 *state = NULL;
State8080 *savestate = NULL;

uint8_t next_interrupt = 1;
uint8_t save_next_interrupt = 1;

SDL_AudioSpec wavSpec;
uint8_t* wavBuffers[18];
uint32_t wavLengths[18];


SDL_AudioDeviceID deviceId = NULL;

/**************************** SDL FUNCTIONS ****************************/

/**
 * @brief initialize video and audio and load audio files
 * 
 * @return true 
 * @return false 
 */
bool init_SDL()
{
    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
        exit(0);
        return false;
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
        exit(0);
        return false;
    }

    deviceId = SDL_OpenAudioDevice(NULL, 0, &wavSpec, NULL, 0);

    for (int i = 0; i < 19; ++i) {
        char *filename = SDL_GetBasePath();
        sprintf(filename, "audio/%d.wav", i);
        if (SDL_LoadWAV(filename, &wavSpec, &wavBuffers[i], &wavLengths[i]) == NULL) {
            fprintf(stderr, "ERROR: %s\n", SDL_GetError());
            
            continue;
        }
    }
    
    return true;
}

/**
 * @brief play the specified wav file
 * 
 * @param index 
 */
void play_wav_file(int index) {
    
    
    if (deviceId == 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
        exit(0);
        // Handle error as needed
        return;
    }

    SDL_QueueAudio(deviceId, wavBuffers[index], wavLengths[index]);
    SDL_PauseAudioDevice(deviceId, 0);

    return;
}

/**
 * @brief free wav file memory
 * 
 */
void cleanup() {
    for (int i = 0; i < 18; ++i) {
        SDL_FreeWAV(wavBuffers[i]);
    }
    SDL_Quit();
}

/**
 * @brief Create a window object
 * 
 * @return SDL_Window* 
 */
SDL_Window *create_window()
{
    int window_width = WIDTH * DISPLAY_SCALE;
    int window_height = HEIGHT * DISPLAY_SCALE;

    SDL_Window *new_window = SDL_CreateWindow(
        "SPACE INVADERS",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        window_width,
        window_height,
        SDL_WINDOW_SHOWN
    );

    if (!new_window)
    {
        fprintf(stderr, "Could not create SDL window: %s\n", SDL_GetError());
        return NULL;
    }

    return new_window;
}

/**
 * @brief set the pixel object
 * 
 * @param x x coordinate
 * @param y y coordinate
 * @param pix color (hex)
 */
void set_pixel(int x, int y, uint32_t pix)
{
    Uint32 *pixels = (Uint32 *)surface->pixels;
    pixels[(y * surface->w) + x] = pix;
}

/**
 * @brief process input and send IN instructions to the emulator
 * 
 * @param state the State8080 object
 */
void process_input(State8080 *state) {
    SDL_Event event;
    SDL_PollEvent(&event);

    if(event.type == SDL_QUIT){
        game_running = false;
        
    }

    if(event.type == SDL_KEYDOWN) {
        if(event.key.keysym.sym == SDLK_ESCAPE) {
            game_running = 0;
            cleanup();
            exit(0);
        }
        if(event.key.keysym.sym == SDLK_c) { // insert coin
            in_port_1 |= 1;
        }
        if(event.key.keysym.sym == SDLK_1) { // P1 Start
            in_port_1 |= (1 << 2);
        }
        if(event.key.keysym.sym == SDLK_SPACE){ // P1 Shoot
            in_port_1 |= (1 << 4);
        }
        if(event.key.keysym.sym == SDLK_a){ // P1 left
            in_port_1 |= (1 << 5);
        }
        if(event.key.keysym.sym == SDLK_d){ // P1 right
            in_port_1 |= (1 << 6);
        }
        if(event.key.keysym.sym == SDLK_2) { // P2 Start
            in_port_1 |= (1 << 1);
        }
        if(event.key.keysym.sym == SDLK_k){ // P2 Shoot
            in_port_2 |= (1 << 4);
        }
        if(event.key.keysym.sym == SDLK_j){ // P2 left
            in_port_2 |= (1 << 5);
        }
        if(event.key.keysym.sym == SDLK_l){ // P2 right
            in_port_2 |= (1 << 6);
        }
       
    }
            
    else if(event.type == SDL_KEYUP) {
        if(event.key.keysym.sym == SDLK_c) { // insert coin
            in_port_1 &= ~1;
        }
        if(event.key.keysym.sym == SDLK_1) { // P1 Start
            in_port_1 &= ~(1 << 2);
        }
        if(event.key.keysym.sym == SDLK_SPACE){ // P1 Shoot
            in_port_1 &= ~(1 << 4);
        }
        if(event.key.keysym.sym == SDLK_a){ // P1 left
            in_port_1 &= ~(1 << 5);
        }
        if(event.key.keysym.sym == SDLK_d){ // P1 right
            in_port_1 &= ~(1 << 6);
        }
        if(event.key.keysym.sym == SDLK_2) { // P2 Start
            in_port_1 &= ~(1 << 1);
        }
        if(event.key.keysym.sym == SDLK_k){ // P2 Shoot
            in_port_2 &= ~(1 << 4);
        }
        if(event.key.keysym.sym == SDLK_j){ // P2 left
            in_port_2 &= ~(1 << 5);
        }
        if(event.key.keysym.sym == SDLK_l){ // P2 right
            in_port_2 &= ~(1 << 6);
        }
    }
    printf("IN PORT: %02x\n", in_port_1);
}

/**
 * @brief renders the game video from VRAM
 * 
 * @param state the State8080 object
 */
void render(State8080 *state) {
    
    // FILE *f = fopen("frame.txt", "w+");
    surface = SDL_GetWindowSurface(window);
    
    bool rotated_screen[224][256];
    bool fixed_screen[256][224];

    
    for (int y = 0; y < WIDTH; y++) {
        for (int x = 0; x < HEIGHT; x++) {
            // printf("INDEX: %d", y + (WIDTH - x - 1) * HEIGHT);
            rotated_screen[y][x] = state->memory[0x2400 + (x / 8 + y * HEIGHT / 8)] & (1 << (x & 7));
        }
    }
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            fixed_screen[y][x] = rotated_screen[x][HEIGHT - y];
            // fprintf(f, "%c", (fixed_screen[y][x] == 1) ? 'X' : ' ');
            // scale up
            for(int i = y * DISPLAY_SCALE; i < (y * DISPLAY_SCALE) + DISPLAY_SCALE; i++) {
                for(int j = x * DISPLAY_SCALE; j < (x * DISPLAY_SCALE) + DISPLAY_SCALE; j++) {
                    set_pixel(j, i, fixed_screen[y][x] ? 0x39ff14 : 0);
                    
                }
            }
        }
        // fprintf(f, "\n");
    }
    

    // fclose(f);
    SDL_UpdateWindowSurface(window);
}

void play_sound() {
    if (sound1_ != last_sound1_) // bit changed
	{
		if ( (sound1_ & 0x2) && !(last_sound1_ & 0x2) )
			play_wav_file(1);
        if ( (sound1_ & 0x4) && !(last_sound1_ & 0x4) )
            play_wav_file(2);
        if ( (sound1_ & 0x8) && !(last_sound1_ & 0x8) )
			play_wav_file(3);
		last_sound1_ = sound1_;
	}
	if (sound2_ != last_sound2_)
	{
		if ( (sound2_ & 0x1) && !(last_sound2_ & 0x1) )
			play_wav_file(4);
		if ( (sound2_ & 0x2) && !(last_sound2_ & 0x2) )
			play_wav_file(5);
		if ( (sound2_ & 0x4) && !(last_sound2_ & 0x4) )
			play_wav_file(6);
		if ( (sound2_ & 0x8) && !(last_sound2_ & 0x8) )
			play_wav_file(7);
		if ( (sound2_ & 0x10) && !(last_sound2_ & 0x10) )
			play_wav_file(8);
		last_sound2_ = sound2_;
	}
}

/**************************** MACHINE I/O FUNCTIONS ****************************/
/**
 * @brief reads data from the specified port
 * 
 * @param port the port to read from
 * @return uint8_t 
 */
static inline uint8_t machine_in(uint8_t port) {
    uint8_t a;    
    switch(port)    
    {   
        case 1:
            a = in_port_1;
            break;
        case 2:
            a = in_port_2;
            break;
        case 3:    
        {    
            uint16_t v = (shift1<<8) | (shift0);    
            a = ((v >> (8-shift_offset)) & 0xff);  
            break;    
        }    
      
    }    
    return a;  
}

/**
 * @brief writes data to the specified port
 * 
 * @param port the port to write to
 * @param value the data to write to the port
 */
static inline void machine_out(uint8_t port, uint8_t value) {
    switch(port)    
    {    
        case 2:    
            shift_offset = value & 0x7;    
            break;    
        case 3:
            sound1_ = value;
            break;
        case 4:    
            shift0 = shift1;    
            shift1 = value;    
            break;  
        case 5:
            sound2_ = value;
            break;  
    }    
    play_sound();
}

int main(void) {
    state = Init8080();
    savestate = Init8080();
    char *romfile = SDL_GetBasePath();
    printf("%s\n", romfile);
    strcat(romfile, "invaders.rom");
    FILE *f = fopen(romfile, "rb"); // open ROM file   

    if (f == NULL) {
        printf("error: Could not open invaders.rom");
        exit(1);
    }

    // get file size, read it into a buffer
    fseek(f, 0L, SEEK_END);
    int fsize = ftell(f);
    fseek(f, 0L, SEEK_SET);

    // read the program into 8080 memory
    fread(state->memory, fsize, 1, f);
    fclose(f);

    state->pc = 0; // set program counter

    bool sdl_working = init_SDL();

    window = create_window();
    game_running = true;

    int interrupt_timing = 33333 / 2;

    // play_wav_file(1);
    // loop through file and read
    while (game_running) {
        
        if(state->pc == 0x0AC2)
            printf("MODE = %d\n", state->memory[0x20c1]);

        if(state->memory[state->pc] == 0xdb) {
            // IN instruction
            printf("PORT: %d\n", state->memory[state->pc + 1]);
            uint8_t port = state->memory[state->pc + 1]; 

            
            state->a = machine_in(port);  

            state->pc += 2; 
            state->cycles += 10;
            
        } 
        else if (state->memory[state->pc] == 0xd3) {
            // OUT instruction
            
            uint8_t port = state->memory[state->pc + 1];  
            
            machine_out(port, state->a); 

            printf("WRITE %02x TO PORT %02X\n", state->a, port); 
            state->pc += 2;  
            state->cycles += 10;
        } 
        else 
            emulate8080Op(state);
        
        
        if(state->cycles > interrupt_timing) {
            generate_interrupt(state, next_interrupt);

            if (interrupt_timing == 33333) {
                render(state);
                process_input(state);
                
            }
            next_interrupt = (next_interrupt == 1) ? 2 : 1;
            interrupt_timing = (interrupt_timing == 33333 / 2) ? 33333 : 33333 / 2;
        }

        if(state->cycles >= 33333)
            state->cycles = 0;
        
       
        
    }   

    return 0;
}
