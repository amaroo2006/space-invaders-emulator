#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "Disassemble8080.h"

#define FOR_CPUDIAG false
#define DEBUG true

typedef struct ConditionCodes {    
    uint8_t z:1; // zero
    uint8_t s:1; // sign
    uint8_t p:1; // parity
    uint8_t cy:1; // carry
    uint8_t ac:1; // auxiliary carry
    uint8_t pad:3; // data
} ConditionCodes;    

typedef struct State8080 {
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
    uint8_t h;
    uint8_t l;
    uint16_t sp;
    uint16_t pc;
    uint8_t *memory; // array of bytes
    struct ConditionCodes cc;
    uint8_t int_enable;
    uint8_t halted;
    uint16_t cycles;
} State8080;

static const uint8_t OPCODES_CYCLES[256] = {
//  0  1   2   3   4   5   6   7   8  9   A   B   C   D   E  F
    4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7, 4,  // 0
    4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7, 4,  // 1
    4, 10, 16, 5,  5,  5,  7,  4,  4, 10, 16, 5,  5,  5,  7, 4,  // 2
    4, 10, 13, 5,  10, 10, 10, 4,  4, 10, 13, 5,  5,  5,  7, 4,  // 3
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,  // 4
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,  // 5
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,  // 6
    7, 7,  7,  7,  7,  7,  7,  7,  5, 5,  5,  5,  5,  5,  7, 5,  // 7
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,  // 8
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,  // 9
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,  // A
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,  // B
    5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7, 11, // C
    5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7, 11, // D
    5, 10, 10, 18, 11, 11, 7,  11, 5, 5,  10, 4,  11, 17, 7, 11, // E
    5, 10, 10, 4,  11, 11, 7,  11, 5, 5,  10, 4,  11, 17, 7, 11  // F
};


int ReadFileIntoMemoryAt(State8080* state, char* filename, uint32_t offset)
{
	FILE *f= fopen(filename, "rb");
	if (f==NULL)
	{
		printf("error: Couldn't open %s\n", filename);
		exit(1);
	}
	fseek(f, 0L, SEEK_END);
	int fsize = ftell(f);
	fseek(f, 0L, SEEK_SET);
	
	uint8_t *buffer = &state->memory[offset];
	fread(buffer, fsize, 1, f);
	fclose(f);

    return fsize;
}


/************************ CONDITION FLAGS UPDATERS ************************/
/**
 * @brief checks the bit parity of a byte
 * 
 * @param value the byte to check parity of
 * @return true 
 * @return false 
 */
static inline bool parity(uint8_t value) {
    uint8_t one_bits = 0;
    for (int i = 0; i < 8; i++) {
        one_bits += ((value >> i) & 1);
    }
    return (one_bits & 1) == 0; // return 1 if even number of one bits, 0 if odd number of one bits
}
/**
 * @brief updates the z (zero), s (sign), and p (parity) condition bits
 * 
 * This function accepts a State8080 objct and an 8 bit value. It checks if 
 * the value equals zero, is negative, and whether the parity is even or odd. 
 * @param state the State8080 object
 * @param value the result of an operation
 */
static inline void update_zsp(State8080 *state, uint8_t value) {
    state->cc.z = (value == 0); // zero
    state->cc.s = (value >> 7); // sign
    state->cc.p = parity(value); // parity
}
/**
 * @brief updates the ac (auxiliary carry) condition bits
 * 
 * This function accepts a State8080 objct and an 8 bit value. The auxiliary carry bit
 * is to be set if a carry from the 3rd bit to the 4th bit occurred as a result of 
 * an arithmetic operation. When called on the result of an arithmetic operation, this
 * function checks if the lower nibble of the byte is equal to zero. If so, an aux. carry
 * must have occurred.  
 * @param bit_no bit_no the bit to check for carry
 * @param a value 1
 * @param b value 2
 * @param cy whether to use carry in addition
 */
static inline bool carry(int bit_no, uint8_t a, uint8_t b, bool cy) {
  int16_t result = a + b + cy;
  int16_t carry = result ^ a ^ b;
  return carry & (1 << bit_no);
}

/************************ LOAD/STORE/MOVE OPERATIONS ************************/
/**
 * @brief writes the specified 16-bit value to the BC register pair
 * 
 * @param state the State8080 object
 * @param value the 16-bit value to be written to BC
 */
static inline void write_bc(State8080 *state, uint16_t value) {
    state->b = (value >> 8);
    state->c = (value & 0xff);
}

/**
 * @brief returns the value stored in the BC register pair
 * 
 * @param state the State8080 object
 * @return uint16_t 
 */
static inline uint16_t read_bc(State8080 *state) {
    return (state->b << 8) + state->c;
}

/**
 * @brief writes the specified 16-bit value to the DE register pair
 * 
 * @param state the State8080 object
 * @param value the 16-bit value to be written to DE
 */
static inline void write_de(State8080 *state, uint16_t value) {
    state->d = (value >> 8);
    state->e = (value & 0xff);
}

/**
 * @brief returns the value stored in the DE register pair
 * 
 * @param state the State8080 object
 * @return uint16_t 
 */
static inline uint16_t read_de(State8080 *state) {
    return (state->d << 8) + state->e;
}

/**
 * @brief writes the specified 16-bit value to the HL register pair
 * 
 * @param state the State8080 object
 * @param value the 16-bit value to be written to HL
 */
static inline void write_hl(State8080 *state, uint16_t value) {
    state->h = (value >> 8);
    state->l = (value & 0xff);
}

/**
 * @brief writes the specified 16-bit value to the HL register pair
 * 
 * @param state the State8080 object
 * @param value the 16-bit value to be written to HL
 */
static inline void write_sp(State8080 *state, uint16_t value) {
    state->sp = value;
}

/**
 * @brief returns the value stored in the HL register pair
 * 
 * @param state the State8080 object
 * @return uint16_t 
 */
static inline uint16_t read_hl(State8080 *state) {
    return (state->h << 8) + state->l;
}

/**
 * @brief stores the value of the A (accumulator) register at the memory 
 * address that equals the value of the BC register
 * 
 * @param state the State8080 object
 */
static inline void stax_b(State8080 *state) {
    uint16_t address = read_bc(state);
    state->memory[address] = state->a;
}

/**
 * @brief stores the value of the A (accumulator) register at the memory 
 * address that equals the value of the DE register
 * 
 * @param state the State8080 object
 */
static inline void stax_d(State8080 *state) {
    uint16_t address = read_de(state);
    state->memory[address] = state->a;
}

/**
 * @brief pushes a value to the stack 
 * Decreases the stack pointer by two bytes, and stores a 16-bit value at the new location of the stack
 * @param state the State8080 object
 * @param value the value to be pushed to the stack
 */
static inline void push(State8080 *state, uint16_t value) {
    state->sp -= 2;
    state->memory[state->sp + 1] = value >> 8;
    state->memory[state->sp] = value & 0xff;
    // printf("NOW ON STACK: %04x\n", (state->memory[state->sp + 1] << 8 | state->memory[state->sp]));
}

/**
 * @brief pops a value that was stored on the stack 
 * Gets the value stored on the stack and increases the stack pointer by two bytes, resetting its original position.
 * @param state the State8080 object
 */
static uint16_t pop(State8080 *state) {
    uint16_t value = (state->memory[state->sp + 1] << 8) | state->memory[state->sp];
    state->sp += 2;
    // printf("NOW ON STACK: %04x\n", (state->memory[state->sp + 1] << 8 | state->memory[state->sp]));
    return value;
}




/************************ ARITHMETIC OPERATIONS ************************/
/**
 * @brief arithmetic operations, including flavors of add, subtract, decrement, and
 * increment
 * 
 */

/**
 * @brief adds the value in the BC register pair to the value in the HL register 
 * pair and updates the carry bit. 
 * 
 * @param state the State8080 object
 */
static inline void dad_b(State8080 *state) {
    state->cc.cy = ((read_hl(state) + (read_bc(state) >> 16)) & 1);
    write_hl(state, read_hl(state) + read_bc(state));
}

/**
 * @brief adds the value in the DE register pair to the value in the HL register 
 * pair and updates the carry bit. 
 * 
 * @param state the State8080 object
 */
static inline void dad_d(State8080 *state) {
    state->cc.cy = (((read_hl(state) + (read_de(state))) >> 16) & 1);
    write_hl(state, read_hl(state) + read_de(state));
}

/**
 * @brief adds the value in the HL register pair to the value in the HL register 
 * pair and updates the carry bit. 
 * 
 * @param state the State8080 object
 */
static inline void dad_h(State8080 *state) {
    state->cc.cy = ((read_hl(state) + (read_hl(state) >> 16)) & 1);
    write_hl(state, read_hl(state) + read_hl(state));
}

/**
 * @brief adds the value in the SP register to the value in the HL register 
 * pair and updates the carry bit. 
 * 
 * @param state the State8080 object
 */
static inline void dad_sp(State8080 *state) {
    state->cc.cy = ((read_hl(state) + (state->sp >> 16)) & 1);
    write_hl(state, read_hl(state) + state->sp);
}

/**
 * @brief adds two bytes with an optional carry added
 * The byte is added to the specified register. The z, s, p, ac, and c bits are updated. 
 * @param state the State8080 object
 * @param reg pointer to the register that is being added to
 * @param val byte to add
 * @param cy optional carry bit to be used in addition
 */
static inline void add(State8080 *state, uint8_t *reg, uint8_t val, bool cy) {
    uint8_t sum = *reg + val + cy;
    update_zsp(state, sum);
    state->cc.cy = carry(8, *reg, val, cy);
    state->cc.ac = carry(4, *reg, val, cy);
    *reg = sum;
}

static inline void subtract(State8080 *state, uint8_t *reg, uint8_t val, bool cy) {
    add(state, reg, ~val, !cy);
    state->cc.cy = !state->cc.cy;
}

/**
 * @brief compares the specified value to the accumulator and updates flags based on the result
 * 
 * @param state the State8080 object
 * @param value the value to compare the accumulator to
 */
static inline void cmp(State8080 *state, uint8_t value) {
   int16_t result = state->a - value;
    state->cc.cy = result >> 8;
    state->cc.ac = ~(state->a ^ result ^ value) & 0x10;
    update_zsp(state, result & 0xFF);
}


/************************ JUMP, CALL, RETURN, AND CONTROL OPERATIONS ************************/

/**
 * @brief jumps to the specified address
 * 
 * @param state the State8080 object
 * @param address the address to jump to
 */
static inline void jmp(State8080 *state, uint16_t address) {
    state->pc = address;
}

/**
 * @brief returns to the last address stored on the stack
 * 
 * @param state the State8080 object
 */
static inline void ret(State8080 *state) {
    state->pc = pop(state); 
    // printf("NEW PC: %04x\n", state->pc);
}

/**
 * @brief calls a subroutine, storing a return address in the stack
 * The program counter is pushed to the stack. The program execution then jumps to the specified address. 
 * @param state the State8080 object
 * @param address the address of the subroutine to call
 */
static inline void call(State8080 *state, uint16_t address) {
    push(state, state->pc);
    jmp(state, address);
}


/**
 * @brief indicates an invalid operation when an illegal or invalid
 * opcode is called
 * 
 * @param state the State8080 object
 */
static inline void unimplemented_instruction (State8080 *state) {
    printf("Error: Unimplemented instruction\n");
} 



/************************ I/O OPERATIONS ************************/

/**
 * @brief executes interrupt that calls routine at the provided address
 * 
 * @param state the State8080 object
 * @param interrupt_num the interrupt address
 */
static inline void generate_interrupt(State8080 *state, int interrupt_num) {
    
    // call the (equivalent of) the reset instruction
    state->pc += 0;
    call(state, interrupt_num * 8);
    // printf("INTERRUPT CALLED\n");

    // disable interrupts
    state->int_enable = 0;
    state->halted = 0;
}



/**
 * @brief emulates an operation of the 8080 given its current state
 * 
 * @param state the State8080 object, or current state of the machine
 * @return int 
 */
int emulate8080Op(State8080 *state) {
    unsigned char *opcode = &state->memory[state->pc];
    if(DEBUG)
        Disassemble8080Op(state->memory, state->pc);   
    state->cycles += OPCODES_CYCLES[*opcode];
    // printf("OPCODE: %02x\n", *opcode); 
    switch(*opcode) 
    {
        case 0x00: state->pc += 1; break; //    NOP
        case 0x01:        //    LXI B, word
        {
            uint16_t value = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            write_bc(state, value);
            state->pc += 3;
            break;
        }
        case 0x02:        //    STAX B
            stax_b(state);
            state->pc += 1;
            break;
            
        case 0x03:        //    INX B
            state->c++;
            if(state->c == 0)
                state->b++;
            state->pc += 1;
            break;
        case 0x04:        //    INR B
            state->b++;
            update_zsp(state, state->b);
            state->cc.ac = (state->b & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x05:        //    DCR B
            state->b--;
            update_zsp(state, state->b);
            state->cc.ac = !((state->b & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x06:        //    MVI B, byte
            state->b = opcode[1];
            state->pc += 2;
            break;
        case 0x07:        //    RLC
            state->cc.cy = state->a >> 7; // set carry bit to high order bit (rotating left)
            state->a = (state->a << 1) | (state->cc.cy);
            state->pc += 1;
            break;
        case 0x08: state->pc += 1; break; //    *NOP
        case 0x09:        //    DAD B
            dad_b(state);
            state->pc += 1;
            break;
        case 0x0a:        //    LDAX B
            state->a = state->memory[read_bc(state)];
            state->pc += 1;
            break;
        case 0x0b:        //    DCX B
            write_bc(state, read_bc(state) - 1);
            state->pc += 1;
            break;
        case 0x0c:        //    INR C
            state->c++;
            update_zsp(state, state->c);
            state->cc.ac = (state->c & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x0d:        //    DCR C
            state->c--;
            update_zsp(state, state->c);
            state->cc.ac = !((state->c & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x0e:        //    MVI C, byte
            state->c = opcode[1];
            state->pc += 2;
            break;
        case 0x0f:        //    RRC
            state->cc.cy = state->a & 1;
            state->a = (state->a >> 1) | (state->cc.cy << 7);
            state->pc += 1;
            break;
        
        case 0x10: state->pc += 1; break; //    NOP
        case 0x11:        //    LXI D, word
        {
            uint16_t value = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            write_de(state, value);
            state->pc += 3;
            break;
        }   
        case 0x12:        //    STAX D
            stax_d(state);
            state->pc += 1;
            break;
        case 0x13:        //    INX D
            write_de(state, read_de(state) + 1);
            state->pc += 1;
            break;
        case 0x14:        //    INR D
            state->d++;
            update_zsp(state, state->d);
            state->cc.ac = (state->d & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x15:        //    DCR D
            state->d--;
            update_zsp(state, state->d);
            state->cc.ac = !((state->d & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x16:        //    MVI D, byte
            state->d = opcode[1];
            state->pc += 2;
            break;
        case 0x17:        //    RAL
        {
            bool cy = state->cc.cy;
            state->cc.cy = (state->a >> 7);
            state->a = (state->a << 1) | cy;
            state->pc += 1;
            break;
        }    
        case 0x18: state->pc += 1; break; //    *NOP
        case 0x19:        //    DAD D
            dad_d(state);
            state->pc += 1;
            break;
        case 0x1a:        //    LDAX D
            state->a = state->memory[read_de(state)];
            state->pc += 1;
            break;
        case 0x1b:        //    DCX D
            write_de(state, read_de(state) - 1);
            state->pc += 1;
            break;
        case 0x1c:        //    INR E
            state->e++;
            update_zsp(state, state->e);
            state->cc.ac = (state->e & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x1d:        //    DCR E
            state->e--;
            update_zsp(state, state->e);
            state->cc.ac = !((state->e & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x1e:        //    MVI E, byte
            state->e = opcode[1];
            state->pc += 2;
            break;
        case 0x1f:        //    RAR
        {
            bool cy = state->cc.cy;
            state->cc.cy = (state->a) & 1;
            state->a = (state->a >> 1) | (cy << 7);
            state->pc += 1;
            break;
        }    
        case 0x20: state->pc += 1; break; //    *NOP
        case 0x21:        //    LXI H, word
        {
            uint16_t value = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            write_hl(state, value);
            state->pc += 3;
            break;
        }    
        case 0x22:        //    SHLD word
        {
            uint16_t address = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            state->memory[address] = state->l;
            state->memory[address + 1] = state->h;
            state->pc += 3;
            break;
        }    
        case 0x23:        //    INX H
            // printf("PREVIOUS HL %04x\n", read_hl(state));
            write_hl(state, read_hl(state) + 1);
            // printf("AFTER INX HL %04x\n", read_hl(state));
            state->pc += 1;
            break;
        case 0x24:        //    INR H
            state->h++;
            update_zsp(state, state->h);
            state->cc.ac = (state->h & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x25:        //    DCR H
            state->h--;
            update_zsp(state, state->h);
            state->cc.ac = !((state->h & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x26:        //    MVI H, byte
            state->h = opcode[1];
            state->pc += 2;
            break;
        case 0x27:        //    DAA
        {
            bool cy = state->cc.cy;
            uint8_t correction = 0;

            uint8_t lsb = state->a & 0x0F;
            uint8_t msb = state->a >> 4;

            if (state->cc.ac || lsb > 9) {
                correction += 0x06;
            }

            if (state->cc.cy || msb > 9 || (msb >= 9 && lsb > 9)) {
                correction += 0x60;
                cy = 1;
            }

            add(state, &state->a, correction, 0);

            state->cc.cy = cy;
            state->pc += 1;
            break;
        }
        case 0x28: state->pc += 1; break; //    *NOP
        case 0x29:        //    DAD H 
            dad_h(state);
            state->pc += 1;
            break;
        case 0x2a:        //    LHLD word
        {
            uint16_t address = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            state->l = state->memory[address];
            state->h = state->memory[address + 1];
            state->pc += 3;
            break;
        }    
        case 0x2b:        //    DCX H
            write_hl(state, read_hl(state) - 1);
            state->pc += 1;
            break;
        case 0x2c:        //    INR L
            state->l++;
            update_zsp(state, state->l);
            state->cc.ac = (state->l & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x2d:        //    DCR L
            state->l--;
            update_zsp(state, state->l);
            state->cc.ac = !((state->l & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x2e:        //    MVI L, byte
            state->l = opcode[1];
            state->pc += 2;
            break;
        case 0x2f:        //    CMA
            state->a = ~state->a;
            state->pc += 1;
            break;
        
        case 0x30: state->pc += 1; break; //    *NOP
        case 0x31:        //    LXI SP, word
        {
            uint16_t value = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            write_sp(state, value);
            state->pc += 3;
            break;
        }    
        case 0x32:        //    STA, word
        {
            uint16_t address = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            state->memory[address] = state->a;
            state->pc += 3;
            break;
        }    
        case 0x33:        //    INX SP
            state->sp++;
            state->pc += 1;
            break;
        case 0x34:        //    INR M 
            state->memory[read_hl(state)] =  state->memory[read_hl(state)] + 1;
            state->cc.ac = (state->memory[read_hl(state)] & 0xf) == 0;
            update_zsp(state, read_hl(state));
            state->pc += 1;
            break;
        case 0x35:        //    DCR M
            state->memory[read_hl(state)] -= 1;
            state->cc.ac = !((state->memory[read_hl(state)] & 0xF) == 0xF);
            update_zsp(state, state->memory[read_hl(state)]);
            state->pc += 1;
            break;
        case 0x36:        //    MVI M, byte
            state->memory[read_hl(state)] = opcode[1];
            state->pc += 2;
            break;
        case 0x37:        //    STC
            state->cc.cy = 1;
            state->pc += 1;
            break;
        case 0x38:        //    *NOP
            state->pc += 1;
            break;
        case 0x39:        //    DAD SP
            dad_sp(state);
            state->pc += 1;
            break;
        case 0x3a:        //    LDA word
        {
            uint16_t address = opcode[1] + (opcode[2] << 8); // combine the two bytes in the correct order
            state->a = state->memory[address];
            state->pc += 3;
            break;
        }    
        case 0x3b:        //    DCX SP 
            state->sp--;
            state->pc += 1;
            break;
        case 0x3c:        //    INR A
            state->a++;
            update_zsp(state, state->a);
            state->cc.ac = (state->a & 0xf) == 0;
            state->pc += 1;
            break;
        case 0x3d:        //    DCR A
            state->a--;
            update_zsp(state, state->a);
            state->cc.ac = !((state->a & 0xF) == 0xF);
            state->pc += 1;
            break;
        case 0x3e:        //    MVI A, byte
            state->a = opcode[1];
            state->pc += 2;
            break;
        case 0x3f:        //    CMC
            state->cc.cy = !state->cc.cy;
            state->pc += 1;
            break;

        case 0x40:        //    MOV B, B
            state->b = state->b;
            state->pc += 1;
            break;
        case 0x41:        //    MOV B, C
            state->b = state->c;
            state->pc += 1;
            break;
        case 0x42:        //    MOV B, D
            state->b = state->d;
            state->pc += 1;
            break;
        case 0x43:        //    MOV B, E
            state->b = state->e;
            state->pc += 1;
            break;
        case 0x44:        //    MOV B, H
            state->b = state->h;
            state->pc += 1;
            break;
        case 0x45:        //    MOV B, L
            state->b = state->l;
            state->pc += 1;
            break;
        case 0x46:        //    MOV B, M
            state->b = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x47:        //    MOV B, A
            state->b = state->a;
            state->pc += 1;
            break;
        case 0x48:        //    MOV C, B
            state->c = state->b;
            state->pc += 1;
            break;
        case 0x49:        //    MOV C, C
            state->c = state->c;
            state->pc += 1;
            break;
        case 0x4a:        //    MOV C, D
            state->c = state->d;
            state->pc += 1;
            break;
        case 0x4b:        //    MOV C, E
            state->c = state->e;
            state->pc += 1;
            break;
        case 0x4c:        //    MOV C, H
            state->c = state->h;
            state->pc += 1;
            break;
        case 0x4d:        //    MOV C, L
            state->c = state->l;
            state->pc += 1;
            break;
        case 0x4e:        //    MOV C, M
            state->c = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x4f:        //    MOV C, A
            state->c = state->a;
            state->pc += 1;
            break;

        case 0x50:        //    MOV D, B
            state->d = state->b;
            state->pc += 1;
            break;
        case 0x51:        //    MOV D, C
            state->d = state->c;
            state->pc += 1;
            break;
        case 0x52:        //    MOV D, D
            state->d = state->d;
            state->pc += 1;
            break;
        case 0x53:        //    MOV D, E
            state->d = state->e;
            state->pc += 1;
            break;
        case 0x54:        //    MOV D, H
            state->d = state->h;
            state->pc += 1;
            break;
        case 0x55:        //    MOV D, L
            state->d = state->l;
            state->pc += 1;
            break;
        case 0x56:        //    MOV D, M
            state->d = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x57:        //    MOV D, A
            state->d = state->a;
            state->pc += 1;
            break;
        case 0x58:        //    MOV E, B
            state->e = state->b;
            state->pc += 1;
            break;
        case 0x59:        //    MOV E, C
            state->e = state->c;
            state->pc += 1;
            break;
        case 0x5a:        //    MOV E, D
            state->e = state->d;
            state->pc += 1;
            break;
        case 0x5b:        //    MOV E, E
            state->e = state->e;
            state->pc += 1;
            break;
        case 0x5c:        //    MOV E, H
            state->e = state->h;
            state->pc += 1;
            break;
        case 0x5d:        //    MOV E, L
            state->e = state->l;
            state->pc += 1;
            break;
        case 0x5e:        //    MOV E, M
            state->e = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x5f:        //    MOV E, A
            state->e = state->a;
            state->pc += 1;
            break;

        case 0x60:        //    MOV H, B
            state->h = state->b;
            state->pc += 1;
            break;
        case 0x61:        //    MOV H, C
            state->h = state->c;
            state->pc += 1;
            break;
        case 0x62:        //    MOV H, D
            state->h = state->d;
            state->pc += 1;
            break;
        case 0x63:        //    MOV H, E
            state->h = state->e;
            state->pc += 1;
            break;
        case 0x64:        //    MOV H, H
            state->h = state->h;
            state->pc += 1;
            break;
        case 0x65:        //    MOV H, L
            state->h = state->l;
            state->pc += 1;
            break;
        case 0x66:        //    MOV H, M
            state->h = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x67:        //    MOV H, A
            state->h = state->a;
            state->pc += 1;
            break;
        case 0x68:        //    MOV L, B
            state->l = state->b;
            state->pc += 1;
            break;
        case 0x69:        //    MOV L, C
            state->l = state->c;
            state->pc += 1;
            break;
        case 0x6a:        //    MOV L, D
            state->l = state->d;
            state->pc += 1;
            break;
        case 0x6b:        //    MOV L, E
            state->l = state->e;
            state->pc += 1;
            break;
        case 0x6c:        //    MOV L, H
            state->l = state->h;
            state->pc += 1;
            break;
        case 0x6d:        //    MOV L, L
            state->l = state->l;
            state->pc += 1;
            break;
        case 0x6e:        //    MOV L, M
            state->l = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x6f:        //    MOV L, A
            state->l = state->a;
            state->pc += 1;
            break;
        
        case 0x70:        //    MOV M, B
            state->memory[read_hl(state)] = state->b;
            state->pc += 1;
            break;
        case 0x71:        //    MOV M, C
            state->memory[read_hl(state)] = state->c;
            state->pc += 1;
            break;
        case 0x72:        //    MOV M, D
            state->memory[read_hl(state)] = state->d;
            state->pc += 1;
            break;
        case 0x73:        //    MOV M, E
            state->memory[read_hl(state)] = state->e;
            state->pc += 1;
            break;
        case 0x74:        //    MOV M, H
            state->memory[read_hl(state)] = state->h;
            state->pc += 1;
            break;
        case 0x75:        //    MOV M, L
            state->memory[read_hl(state)] = state->l;
            state->pc += 1;
            break;
        case 0x76:        //    HLT
            state->halted = 1;
            state->pc += 1;
            break;
        case 0x77:        //    MOV M, A
            state->memory[read_hl(state)] = state->a;
            state->pc += 1;
            break;
        case 0x78:        //    MOV A, B
            state->a = state->b;
            state->pc += 1;
            break;
        case 0x79:        //    MOV A, C
            state->a = state->c;
            state->pc += 1;
            break;
        case 0x7a:        //    MOV A, D
            state->a = state->d;
            state->pc += 1;
            break;
        case 0x7b:        //    MOV A, E
            state->a = state->e;
            state->pc += 1;
            break;
        case 0x7c:        //    MOV A, H
            state->a = state->h;
            state->pc += 1;
            break;
        case 0x7d:        //    MOV A, L
            state->a = state->l;
            state->pc += 1;
            break;
        case 0x7e:        //    MOV A, M
            state->a = state->memory[read_hl(state)];
            state->pc += 1;
            break;
        case 0x7f:        //    MOV A, A
            state->a = state->a;
            state->pc += 1;
            break;
        
        case 0x80:        //    ADD B
            add(state, &state->a, state->b, 0);
            state->pc += 1;
            break;
        case 0x81:        //    ADD C
            add(state, &state->a, state->c, 0);
            state->pc += 1;
            break;
        case 0x82:        //    ADD D
            add(state, &state->a, state->d, 0);
            state->pc += 1;
            break;
        case 0x83:        //    ADD E
            add(state, &state->a, state->e, 0);
            state->pc += 1;
            break;
        case 0x84:        //    ADD H
            add(state, &state->a, state->h, 0);
            state->pc += 1;
            break;
        case 0x85:        //    ADD L
            add(state, &state->a, state->l, 0);
            state->pc += 1;
            break;
        case 0x86:        //    ADD M
            add(state, &state->a, state->memory[read_hl(state)], 0);
            state->pc += 1;
            break;
        case 0x87:        //    ADD A
            add(state, &state->a, state->a, 0);
            state->pc += 1;
            break;
        case 0x88:        //    ADC B
            add(state, &state->a, state->b, state->cc.cy);
            state->pc += 1;
            break;
        case 0x89:        //    ADC C
            add(state, &state->a, state->c, state->cc.cy);
            state->pc += 1;
            break;
        case 0x8a:        //    ADC D
            add(state, &state->a, state->d, state->cc.cy);
            state->pc += 1;
            break;
        case 0x8b:        //    ADC E
            add(state, &state->a, state->e, state->cc.cy);
            state->pc += 1;
            break;
        case 0x8c:        //    ADC H
            add(state, &state->a, state->h, state->cc.cy);
            state->pc += 1;
            break;
        case 0x8d:        //    ADC L
            add(state, &state->a, state->l, state->cc.cy);
            state->pc += 1;
            break;
        case 0x8e:        //    ADC M
            add(state, &state->a, state->memory[read_hl(state)], state->cc.cy);
            state->pc += 1;
            break;
        case 0x8f:        //    ADC A
            add(state, &state->a, state->a, state->cc.cy);
            state->pc += 1;
            break;
        
        case 0x90:        //    SUB B 
            subtract(state, &state->a, state->b, 0);
            state->pc += 1;
            break;
        case 0x91:        //    SUB C 
            subtract(state, &state->a, state->c, 0);
            state->pc += 1;
            break;
        case 0x92:        //    SUB D 
            subtract(state, &state->a, state->d, 0);
            state->pc += 1;
            break;
        case 0x93:        //    SUB E
            subtract(state, &state->a, state->e, 0);
            state->pc += 1;
            break;
        case 0x94:        //    SUB H 
            subtract(state, &state->a, state->h, 0);
            state->pc += 1;
            break;
        case 0x95:        //    SUB L 
            subtract(state, &state->a, state->l, 0);
            state->pc += 1;
            break;
        case 0x96:        //    SUB M 
            subtract(state, &state->a, state->memory[read_hl(state)], 0);
            state->pc += 1;
            break;
        case 0x97:        //    SUB A 
            subtract(state, &state->a, state->a, 0);
            state->pc += 1;
            break;
        case 0x98:        //    SBB B
            subtract(state, &state->a, state->b, state->cc.cy);
            state->pc += 1;
            break;
        case 0x99:        //    SBB C 
            subtract(state, &state->a, state->c, state->cc.cy);
            state->pc += 1;
            break;
        case 0x9a:        //    SBB D
            subtract(state, &state->a, state->d, state->cc.cy);
            state->pc += 1;
            break;
        case 0x9b:        //    SBB E
            subtract(state, &state->a, state->e, state->cc.cy);
            state->pc += 1;
            break;
        case 0x9c:        //    SBB H
            subtract(state, &state->a, state->h, state->cc.cy);
            state->pc += 1;
            break;
        case 0x9d:        //    SBB L
            subtract(state, &state->a, state->l, state->cc.cy);
            state->pc += 1;
            break;  
        case 0x9e:        //    SBB M
            subtract(state, &state->a, state->memory[read_hl(state)], state->cc.cy);
            state->pc += 1;
            break; 
        case 0x9f:        //    SBB A
            subtract(state, &state->a, state->a, state->cc.cy);
            state->pc += 1;
            break;  
        
        case 0xa0:        //    ANA B
        {
            uint8_t val = state->a &= state->b;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa1:        //    ANA C
        {
            uint8_t val = state->a &= state->c;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa2:        //    ANA D
        {
            uint8_t val = state->a &= state->d;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa3:        //    ANA E
        {
            uint8_t val = state->a &= state->e;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa4:        //    ANA H
        {
            uint8_t val = state->a &= state->h;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa5:        //    ANA L
        {
            uint8_t val = state->a &= state->l;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa6:        //    ANA M
        {
            uint8_t val = state->a &= state->memory[read_hl(state)];
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa7:        //    ANA A
        {
            uint8_t val = state->a &= state->a;
            update_zsp(state, val);
            state->cc.ac = ((state->a | val) & 0x08) != 0;
            state->cc.cy = 0;
            state->a = val;
            state->pc += 1;
            break;
        }
        case 0xa8:        //    XRA B
            state->a ^= state->b;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xa9:        //    XRA C
            state->a ^= state->c;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xaa:        //    XRA D
            state->a ^= state->d;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xab:        //    XRA E
            state->a ^= state->e;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xac:        //    XRA H
            state->a ^= state->h;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xad:        //    XRA L
            state->a ^= state->l;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xae:        //    XRA M
            state->a ^= state->memory[read_hl(state)];
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xaf:        //    XRA A
            state->a ^= state->a;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;

        case 0xb0:        //    ORA B
            state->a |= state->b;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb1:        //    ORA C
            state->a |= state->c;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb2:        //    ORA D
            state->a |= state->d;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb3:        //    ORA E
            state->a |= state->e;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb4:        //    ORA H
            state->a |= state->h;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb5:        //    ORA L
            state->a |= state->l;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb6:        //    ORA M
            state->a |= state->memory[read_hl(state)];
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb7:        //    ORA A
            state->a |= state->a;
            state->cc.cy = 0;
            state->cc.ac = 0;
            update_zsp(state, state->a);
            state->pc += 1;
            break;
        case 0xb8:        //    CMP B
        {
            cmp(state, state->b);
            state->pc += 1;
            break;
        }    
        case 0xb9:        //    CMP C
        {   
            cmp(state, state->c);
            state->pc += 1;
            break;
        }
        case 0xba:        //    CMP D
        {   
            cmp(state, state->d);
            state->pc += 1;
            break;
        }
        case 0xbb:        //    CMP E
        {  
            cmp(state, state->e);
            state->pc += 1;
            break;
        }
        case 0xbc:        //    CMP H
        {   
            cmp(state, state->h);
            state->pc += 1;
            break;
        }
        case 0xbd:        //    CMP L
        {   
            cmp(state, state->l);
            state->pc += 1;
            break;
        }
        case 0xbe:        //    CMP M
        {   
            cmp(state, state->memory[read_hl(state)]);
            state->pc += 1;
            break;
        }
        case 0xbf:        //    CMP A
        {   
            cmp(state, state->a);
            state->pc += 1;
            break;
        }
        case 0xc0:        //    RNZ
            if (state->cc.z == 0)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xc1:        //    POP B
            write_bc(state, pop(state));
            state->pc += 1;
            break;
        case 0xc2:        //    JNZ word
            if (state->cc.z == 0)
                jmp(state, ((opcode[2] << 8) | opcode[1]));
            else
                state->pc += 3;
            break;
        case 0xc3:        //    JMP word
            jmp(state, ((opcode[2] << 8) | opcode[1]));
            break;
        case 0xc4:        //    CNZ
            state->pc += 3;
            if(state->cc.z == 0)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xc5:        //    PUSH B
            push(state, read_bc(state));
            state->pc += 1;
            break;
        case 0xc6:        //    ADI byte
            add(state, &state->a, opcode[1], 0);
            // printf("PARITY OF A: %02x\n", parity(state->a));
            // printf("PARITY OF 0x76: %02x\n", parity(0x76));
            state->pc += 2;
            break;
        case 0xc7:        //    RST 0
            call(state, 0x00);
            // state->pc += 1;
            break;
        case 0xc8:        //    RZ
            if(state->cc.z)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xc9:        //    RET
            ret(state);
            break;
        case 0xca:        //    JZ word
            if(state->cc.z)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xcb:        //    JMP word
            jmp(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xcc:        //    CZ word
            state->pc += 3;
            if(state->cc.z)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xcd:        //    CALL word
            if(FOR_CPUDIAG) {
                if (5 ==  ((opcode[2] << 8) | opcode[1]))    
                {    
                    if (state->c == 9)    
                    {    
                        uint16_t offset = (state->d<<8) | (state->e);    
                        char *str = &state->memory[offset + 3];  //skip the prefix bytes    
                        bool failed = true;
                        while (*str != '$') {
                            printf("%c", *str++);    
                            if(*str == 'F')
                                failed = true;
                        }
                        printf("\n");   
                        if(failed)
                            getchar(); 
                    }    
                    else if (state->c == 2)    
                    {    
                        //saw this in the inspected code, never saw it called    
                        printf ("print char routine called\n");    
                    }  
                    
                    
                }    
                else if (0 ==  ((opcode[2] << 8) | opcode[1]))    
                {    
                    exit(0); 
                }    
            }
            state->pc += 3;
            call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xce:        //    ACI byte
            add(state, &state->a, opcode[1], state->cc.cy);
            state->pc += 2;
            break;
        case 0xcf:        //    RST 1
            state->pc += 1;
            call(state, 0x08);
            break;
        
        case 0xd0:        //    RNC
            if(!state->cc.cy)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xd1:        //    POP D
            write_de(state, pop(state));
            state->pc += 1;
            break;
        case 0xd2:        //    JNC word
            if(!state->cc.cy)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xd3:        //    OUT byte
            unimplemented_instruction(state);
            state->pc += 2;
            break;
        case 0xd4:        //    CNC word
            state->pc += 3;
            if(!state->cc.cy)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xd5:        //    PUSH D
            push(state, read_de(state));
            state->pc += 1;
            break;
        case 0xd6:        //    SUI byte
            subtract(state, &state->a, opcode[1], 0);
            state->pc += 2;
            break;
        case 0xd7:        //    RST 2
            state->pc += 1;
            call(state, 0x10);
            break;
        case 0xd8:        //    RC 
            if(state->cc.cy)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xd9:        //    *RET
            ret(state);
            state->pc += 1;
            break;
        case 0xda:        //    JC word
            if(state->cc.cy)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xdb:        //    IN byte
            unimplemented_instruction(state);
            state->pc += 2;
            break;
        case 0xdc:        //    CC word
            state->pc += 3;
            if(state->cc.cy)
                call(state, (opcode[2] << 8) | opcode[1]);
            // printf("FINISHED EXECUTION OF OPERATION CC\n");
            break;
        case 0xdd:        //    *CALL word
            state->pc += 3;
            call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xde:        //    SBI byte
            subtract(state, &state->a, opcode[1], state->cc.cy);
            state->pc += 2;
            break;
        case 0xdf:        //    RST 3
            state->pc += 1;
            call(state, 0x18);
            break;
        
        case 0xe0:        //    RPO
            if(state->cc.p == 0)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xe1:        //    POP H
            write_hl(state, pop(state));
            state->pc += 1;
            break;
        case 0xe2:        //    JPO word
            state->pc += 3;
            if(state->cc.p == 0)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xe3:        //    XTHL
        {
            uint16_t val = (state->memory[state->sp + 1] << 8) | state->memory[state->sp];
            state->memory[state->sp + 1] = state->h;
            state->memory[state->sp] = state->l;
            write_hl(state, val);
            state->pc += 1;
            break;
        }
        case 0xe4:        //    CPO word
            state->pc += 3;
            if(state->cc.p == 0)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xe5:        //    PUSH H
            push(state, read_hl(state));
            state->pc += 1;
            break;
        case 0xe6:        //    ANI byte
            state->a &= opcode[1];
            update_zsp(state, state->a);
            state->cc.cy = 0;
            state->pc += 2;
            break;
        case 0xe7:        //    RST 4
            state->pc += 1;
            call(state, 0x20);
            break;
        case 0xe8:        //    RPE 
            if(state->cc.p)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xe9:        //    PCHL
            state->pc = read_hl(state);
            break;
        case 0xea:        //    JPE word
            if(state->cc.p)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xeb:        //    XCHG word
        {   uint16_t hl = read_hl(state);
            write_hl(state, read_de(state));
            write_de(state, hl);
            state->pc += 1;
            break;
        }
        case 0xec:        //    CPE word
            state->pc += 3;
            if(state->cc.p)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;  
        case 0xed:        //    *CALL word
            state->pc += 3;
            call(state, (opcode[2] << 8) | opcode[1]);
            break;  
        case 0xee:        //    XRI byte
            state->a ^= opcode[1];
            update_zsp(state, state->a);
            state->cc.ac = 0;
            state->cc.cy = 0;
            state->pc += 2;
            break;
        case 0xef:        //    RST 5
            state->pc += 1;
            call(state, 0x28);
            break;

        case 0xf0:        //    RP 
            if(state->cc.s == 0)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xf1:        //    POP PSW 
        {   
            uint16_t af = pop(state);
            state->a = (af >> 8);
            uint8_t flags = af & 0xff;
            state->cc.s = (flags >> 7) & 1;
            state->cc.z = (flags >> 6) & 1;
            state->cc.ac = (flags >> 4) & 1;
            state->cc.p = (flags >> 2) & 1;
            state->cc.cy = (flags >> 0) & 1;
            state->pc += 1;
            break;
        }
        case 0xf2:        //    JP word 
            if(state->cc.s == 0)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xf3:        //    DI 
            state->int_enable = 0;
            state->pc += 1;
            break;
        case 0xf4:        //    CP word
            state->pc += 3;
            if(state->cc.s == 0)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xf5:        //    PUSH PSW
        {
            uint8_t flags = 0x00;
            flags |= (state->cc.s << 7);
            flags |= (state->cc.z << 6);
            flags |= (state->cc.ac << 4);
            flags |= (state->cc.p << 2);
            flags |= (1 << 1);
            flags |= (state->cc.cy << 0);
            push(state, ((state->a << 8) | flags));
            state->pc += 1;
            break;
        }
        case 0xf6:        //    ORI byte
            state->a |= opcode[1];
            state->cc.cy = 0;
            update_zsp(state, state->a);
            state->pc += 2;
            break;
        case 0xf7:        //    RST 6
            state->pc += 1;
            call(state, 0x30);
            break;
        case 0xf8:        //    RM
            if(state->cc.s)
                ret(state);
            else
                state->pc += 1;
            break;
        case 0xf9:        //    SPHL
            state->sp = read_hl(state);
            state->pc += 1;
            break;
        case 0xfa:        //    JM word
            if(state->cc.s)
                jmp(state, (opcode[2] << 8) | opcode[1]);
            else
                state->pc += 3;
            break;
        case 0xfb:        //    EI
            state->int_enable = 1;
            state->pc += 1;
            break;
        case 0xfc:        //    CM word
            state->pc += 3;
            if(state->cc.s)
                call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xfd:        //    *CALL word
            state->pc += 3;
            call(state, (opcode[2] << 8) | opcode[1]);
            break;
        case 0xfe:        //    CPI byte
            cmp(state, opcode[1]);
            state->pc += 2;
            break;  
        case 0xff:        //    RST 7
            state->pc += 1;
            call(state, 0x38);
            break;

        default: state->pc += 1; unimplemented_instruction(state); break;
    } 
    
    uint8_t flags = 0;
    flags |= state->cc.s << 7;
    flags |= state->cc.z << 6;
    flags |= state->cc.ac << 4;
    flags |= state->cc.p << 2;
    flags |= 1 << 1; // bit 1 is always 1
    flags |= state->cc.cy << 0;

    /* print out processor state */    
    if(DEBUG) {
        printf("\tCY=%d,P=%d,S=%d,Z=%d,AC=%d,INT_EN=%d\n", state->cc.cy, state->cc.p,    
            state->cc.s, state->cc.z, state->cc.ac, state->int_enable);    
        printf("\tAF $%02x%02x BC $%02x%02x DE $%02x%02x HL $%02x%02x SP %04x PC %04x\n",    
            state->a, flags, state->b, state->c, state->d,    
            state->e, state->h, state->l, state->sp, state->pc);    
    }
    // printf("MEMORY: %02x", state->memory[state->pc]);
    return 0;
}

State8080 *Init8080(void) {
    State8080 *state = malloc(sizeof(State8080));
    state->a = 0;
    state->b = 0;
    state->c = 0;
    state->d = 0;
    state->e = 0;
    state->h = 0;
    state->l = 0;
    state->cc.z = 0;
    state->cc.s = 0;
    state->cc.p = 0;
    state->cc.ac = 0;
    state->cc.cy = 0;
    state->cc.pad = 0;
    state->int_enable = 0;
	state->memory = malloc(0x10000); //allocate 16K
    state->cycles = 0;
	return state;
}


