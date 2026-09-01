#include <fenv.h>
#include <stddef.h>
#include <stdint.h>

#define X86_FX_STATE_SIZE          512U
#define X87_EXCEPTION_MASK         0x003FU
#define X87_ROUND_MASK             0x0C00U
#define X87_DEFAULT_CONTROL        0x037FU
#define X86_MXCSR_EXCEPTION_MASK   (X87_EXCEPTION_MASK << 7U)
#define X86_MXCSR_ROUND_MASK       (X87_ROUND_MASK << 3U)
#define X86_MXCSR_DEFAULT          0x00001F80U
#define X86_MXCSR_FALLBACK_MASK    0x0000FFBFU

typedef struct __attribute__((aligned(16))) x86_fx_state {
    uint16_t control;
    uint16_t status;
    uint8_t tag;
    uint8_t reserved;
    uint16_t opcode;
    uint64_t instruction_pointer;
    uint64_t data_pointer;
    uint32_t mxcsr;
    uint32_t mxcsr_mask;
    uint8_t registers[X86_FX_STATE_SIZE - 32U];
} x86_fx_state_t;

_Static_assert(sizeof(x86_fx_state_t) == X86_FX_STATE_SIZE,
               "FXSAVE layout size");
_Static_assert(_Alignof(x86_fx_state_t) >= 16U, "FXSAVE alignment");
_Static_assert(offsetof(x86_fx_state_t, mxcsr) == 24U, "FXSAVE MXCSR offset");

static void x86_fxsave(x86_fx_state_t *state) {
    __asm__ volatile ("fxsave64 (%0)" : : "r"(state) : "memory");
}

static void x86_fxrstor(const x86_fx_state_t *state) {
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(state) : "memory");
}

static uint32_t x86_mxcsr_mask(const x86_fx_state_t *state) {
    return state->mxcsr_mask != 0U ?
           state->mxcsr_mask : X86_MXCSR_FALLBACK_MASK;
}

static uint32_t fenv_excepts(int excepts) {
    return (uint32_t)excepts & (uint32_t)FE_ALL_EXCEPT;
}

static void fenv_capture(fenv_t *environment) {
    x86_fx_state_t state;
    x86_fxsave(&state);
    environment->x87_control = state.control;
    environment->x87_status = state.status;
    environment->mxcsr = state.mxcsr;
}

static void fenv_restore(const fenv_t *environment) {
    x86_fx_state_t state;
    uint32_t mxcsr_mask;

    x86_fxsave(&state);
    mxcsr_mask = x86_mxcsr_mask(&state);
    state.control = environment->x87_control;
    state.status = environment->x87_status;
    state.mxcsr = (state.mxcsr & ~mxcsr_mask) |
                  (environment->mxcsr & mxcsr_mask);
    x86_fxrstor(&state);
}

static uint32_t fenv_current_flags(void) {
    x86_fx_state_t state;
    x86_fxsave(&state);
    return ((uint32_t)state.status | state.mxcsr) &
           (uint32_t)FE_ALL_EXCEPT;
}

static int fenv_valid_round(int round) {
    return round == FE_TONEAREST || round == FE_DOWNWARD ||
           round == FE_UPWARD || round == FE_TOWARDZERO;
}

int feclearexcept(int excepts) {
    x86_fx_state_t state;
    uint32_t mask = fenv_excepts(excepts);

    if (mask == 0U) return 0;
    x86_fxsave(&state);
    state.status &= (uint16_t)~mask;
    state.mxcsr &= ~mask;
    x86_fxrstor(&state);
    return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts) {
    if (flagp == 0) return 1;
    *flagp = (fexcept_t)(fenv_current_flags() & fenv_excepts(excepts));
    return 0;
}

int feraiseexcept(int excepts) {
    x86_fx_state_t state;
    uint32_t mask = fenv_excepts(excepts);

    if (mask == 0U) return 0;
    x86_fxsave(&state);
    state.status |= (uint16_t)mask;
    state.mxcsr |= mask;
    x86_fxrstor(&state);
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) {
    x86_fx_state_t state;
    uint32_t mask;
    uint32_t flags;

    if (flagp == 0) return 1;
    mask = fenv_excepts(excepts);
    if (mask == 0U) return 0;
    flags = *flagp & mask;
    x86_fxsave(&state);
    state.status = (uint16_t)((state.status & (uint16_t)~mask) | flags);
    state.mxcsr = (state.mxcsr & ~mask) | flags;
    x86_fxrstor(&state);
    return 0;
}

int fetestexcept(int excepts) {
    return (int)(fenv_current_flags() & fenv_excepts(excepts));
}

int fegetround(void) {
    x86_fx_state_t state;
    x86_fxsave(&state);
    return (int)(state.control & X87_ROUND_MASK);
}

int fesetround(int round) {
    x86_fx_state_t state;

    if (!fenv_valid_round(round)) return 1;
    x86_fxsave(&state);
    state.control = (uint16_t)((state.control & ~X87_ROUND_MASK) |
                               (uint16_t)round);
    state.mxcsr = (state.mxcsr & ~X86_MXCSR_ROUND_MASK) |
                  ((uint32_t)round << 3U);
    x86_fxrstor(&state);
    return 0;
}

int fegetenv(fenv_t *envp) {
    if (envp == 0) return 1;
    fenv_capture(envp);
    return 0;
}

int feholdexcept(fenv_t *envp) {
    x86_fx_state_t state;

    if (envp == 0) return 1;
    x86_fxsave(&state);
    envp->x87_control = state.control;
    envp->x87_status = state.status;
    envp->mxcsr = state.mxcsr;
    state.control |= X87_EXCEPTION_MASK;
    state.status &= (uint16_t)~X87_EXCEPTION_MASK;
    state.mxcsr = (state.mxcsr | X86_MXCSR_EXCEPTION_MASK) &
                  ~(uint32_t)X87_EXCEPTION_MASK;
    x86_fxrstor(&state);
    return 0;
}

int fesetenv(const fenv_t *envp) {
    static const fenv_t default_environment = {
        X87_DEFAULT_CONTROL,
        0U,
        X86_MXCSR_DEFAULT,
    };

    if (envp == FE_DFL_ENV) envp = &default_environment;
    if (envp == 0) return 1;
    fenv_restore(envp);
    return 0;
}

int feupdateenv(const fenv_t *envp) {
    uint32_t flags;

    if (envp == 0) return 1;
    flags = fenv_current_flags();
    if (fesetenv(envp) != 0) return 1;
    return feraiseexcept((int)flags);
}
