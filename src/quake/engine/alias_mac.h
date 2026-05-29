#ifndef ALIAS_MAC_H
#define ALIAS_MAC_H

/*
 * Alias Transform MAC - FPGA hardware accelerator for alias model vertices.
 * Offloads 3x4 matrix transform + lighting DotProducts to hardware.
 * CPU handles perspective division (fdiv) and screen projection.
 */

/* Force CPU-side alias vertex transform. The ATM MMIO coprocessor
 * below lives at 0x58000000 on Quake's custom core — not on the
 * openfpgaOS fabric. Leaving this on made every alias vertex project
 * to INT32_MAX (garbage reads from the non-existent MMIO), which made
 * every triangle degenerate and every backface-cull fail. */
#ifndef HW_ALIAS_MAC
#define HW_ALIAS_MAC 0
#endif

#define ATM_BASE          0x58000000u

/* Control registers */
#define ATM_MAT(row,col)  (*(volatile unsigned int *)(ATM_BASE + (row)*0x10 + (col)*4))
#define ATM_LIGHT_X       (*(volatile unsigned int *)(ATM_BASE + 0x30))
#define ATM_LIGHT_Y       (*(volatile unsigned int *)(ATM_BASE + 0x34))
#define ATM_LIGHT_Z       (*(volatile unsigned int *)(ATM_BASE + 0x38))
#define ATM_LIGHT_PARAMS  (*(volatile unsigned int *)(ATM_BASE + 0x3C))
#define ATM_VERT_IN       (*(volatile unsigned int *)(ATM_BASE + 0x40))
#define ATM_RESULT_VX     (*(volatile int *)(ATM_BASE + 0x44))
#define ATM_RESULT_VY     (*(volatile int *)(ATM_BASE + 0x48))
#define ATM_RESULT_VZ     (*(volatile int *)(ATM_BASE + 0x4C))
#define ATM_RESULT_LIGHT  (*(volatile int *)(ATM_BASE + 0x50))
#define ATM_STATUS        (*(volatile unsigned int *)(ATM_BASE + 0x54))

/* Normal table: 162 entries, 2 words each.
 * Word 0: {ny[31:16], nx[15:0]}, Word 1: {unused[31:16], nz[15:0]}
 * Address: ATM_BASE + 0x1000 + entry*8 + word*4 */
#define ATM_NORM_W0(idx)  (*(volatile unsigned int *)(ATM_BASE + 0x1000 + (idx)*8))
#define ATM_NORM_W1(idx)  (*(volatile unsigned int *)(ATM_BASE + 0x1000 + (idx)*8 + 4))

static inline int atm_float_to_q16(float f)
{
    return (int)(f * 65536.0f);
}

static inline void atm_load_matrix(float mat[3][4])
{
    int row, col;
    for (row = 0; row < 3; row++)
        for (col = 0; col < 4; col++)
            ATM_MAT(row, col) = (unsigned int)atm_float_to_q16(mat[row][col]);
}

static inline void atm_load_lighting(float *lightvec, int ambient, int shade)
{
    ATM_LIGHT_X = (unsigned int)atm_float_to_q16(lightvec[0]);
    ATM_LIGHT_Y = (unsigned int)atm_float_to_q16(lightvec[1]);
    ATM_LIGHT_Z = (unsigned int)atm_float_to_q16(lightvec[2]);
    ATM_LIGHT_PARAMS = (unsigned int)((ambient & 0xFFFF) | ((shade & 0xFFFF) << 16));
}

static inline void atm_load_normals(float normals[][3], int count)
{
    int i;
    for (i = 0; i < count; i++) {
        short nx = (short)(normals[i][0] * 32767.0f);
        short ny = (short)(normals[i][1] * 32767.0f);
        short nz = (short)(normals[i][2] * 32767.0f);
        ATM_NORM_W0(i) = (unsigned int)(((unsigned short)ny << 16) | (unsigned short)nx);
        ATM_NORM_W1(i) = (unsigned int)((unsigned short)nz);
    }
}

#endif /* ALIAS_MAC_H */
