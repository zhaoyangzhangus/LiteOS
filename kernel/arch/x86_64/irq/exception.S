    .text
    .p2align 4

/*
 * 所有异常都被规范化为 arch_trap_frame_t：
 *   通用寄存器、向量号、错误码、CPU 返回帧。
 * 没有硬件错误码的异常由入口补一个零，这样 C 代码不需要区分两种栈布局。
 */
    .macro EXCEPTION_NO_ERROR vector
    .p2align 4
    .globl arch_exception_\vector
arch_exception_\vector:
    pushq $0
    pushq $\vector
    jmp arch_exception_common
    .endm

    .macro EXCEPTION_WITH_ERROR vector
    .p2align 4
    .globl arch_exception_\vector
arch_exception_\vector:
    pushq $\vector
    jmp arch_exception_common
    .endm

EXCEPTION_NO_ERROR   0
EXCEPTION_NO_ERROR   1
EXCEPTION_NO_ERROR   2
EXCEPTION_NO_ERROR   3
EXCEPTION_NO_ERROR   4
EXCEPTION_NO_ERROR   5
EXCEPTION_NO_ERROR   6
EXCEPTION_NO_ERROR   7
EXCEPTION_WITH_ERROR 8
EXCEPTION_NO_ERROR   9
EXCEPTION_WITH_ERROR 10
EXCEPTION_WITH_ERROR 11
EXCEPTION_WITH_ERROR 12
EXCEPTION_WITH_ERROR 13
EXCEPTION_WITH_ERROR 14
EXCEPTION_NO_ERROR   15
EXCEPTION_NO_ERROR   16
EXCEPTION_WITH_ERROR 17
EXCEPTION_NO_ERROR   18
EXCEPTION_NO_ERROR   19
EXCEPTION_NO_ERROR   20
EXCEPTION_WITH_ERROR 21
EXCEPTION_NO_ERROR   22
EXCEPTION_NO_ERROR   23
EXCEPTION_NO_ERROR   24
EXCEPTION_NO_ERROR   25
EXCEPTION_NO_ERROR   26
EXCEPTION_NO_ERROR   27
EXCEPTION_NO_ERROR   28
EXCEPTION_WITH_ERROR 29
EXCEPTION_WITH_ERROR 30
EXCEPTION_NO_ERROR   31

    .p2align 4
arch_exception_common:
    /* 来自 Ring3 时切换到内核 GS；返回前再切换回来。 */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    movq %rsp, %rdi
    andq $-16, %rsp
    call x86_interrupt_dispatch
    movq %r12, %rsp

    /* frame->cs 位于规范陷阱帧的第 18 个 64 位字段。 */
    testb $3, 144(%rsp)
    jz 2f
    swapgs
2:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

/* 尚未注册的外部中断属于内核配置错误，保持停机以避免中断风暴。 */
    .p2align 4
    .globl arch_arch_interrupt_stub
arch_arch_interrupt_stub:
    cli
3:
    hlt
    jmp 3b

/* C 初始化 IDT 时通过此只读表取得每个异常入口。 */
#ifdef __ELF__
    .section .rdata,"a",@progbits
#else
    .section .rdata,"dr"
#endif
    .p2align 3
    .globl arch_exception_stub_table
arch_exception_stub_table:
    .quad arch_exception_0,  arch_exception_1
    .quad arch_exception_2,  arch_exception_3
    .quad arch_exception_4,  arch_exception_5
    .quad arch_exception_6,  arch_exception_7
    .quad arch_exception_8,  arch_exception_9
    .quad arch_exception_10, arch_exception_11
    .quad arch_exception_12, arch_exception_13
    .quad arch_exception_14, arch_exception_15
    .quad arch_exception_16, arch_exception_17
    .quad arch_exception_18, arch_exception_19
    .quad arch_exception_20, arch_exception_21
    .quad arch_exception_22, arch_exception_23
    .quad arch_exception_24, arch_exception_25
    .quad arch_exception_26, arch_exception_27
    .quad arch_exception_28, arch_exception_29
    .quad arch_exception_30, arch_exception_31

    .section .text
    .p2align 4
    .extern x86_irq_dispatch

    /*
     * 外部设备中断统一构造成 arch_trap_frame_t。
     * 32 号定时器、SMP IPI 和 TLB IPI 保留各自的上下文切换入口。
     */
    .macro IRQ_NO_ERROR vector
    .p2align 4
    .globl arch_irq_\vector
arch_irq_\vector:
    pushq $0
    pushq $\vector
    jmp arch_irq_common
    .endm

IRQ_NO_ERROR   33
IRQ_NO_ERROR   34
IRQ_NO_ERROR   35
IRQ_NO_ERROR   36
IRQ_NO_ERROR   37
IRQ_NO_ERROR   38
IRQ_NO_ERROR   39
IRQ_NO_ERROR   40
IRQ_NO_ERROR   41
IRQ_NO_ERROR   42
IRQ_NO_ERROR   43
IRQ_NO_ERROR   44
IRQ_NO_ERROR   45
IRQ_NO_ERROR   46
IRQ_NO_ERROR   47
IRQ_NO_ERROR   48
IRQ_NO_ERROR   49
IRQ_NO_ERROR   50
IRQ_NO_ERROR   51
IRQ_NO_ERROR   52
IRQ_NO_ERROR   53
IRQ_NO_ERROR   54
IRQ_NO_ERROR   55
IRQ_NO_ERROR   56
IRQ_NO_ERROR   57
IRQ_NO_ERROR   58
IRQ_NO_ERROR   59
IRQ_NO_ERROR   60
IRQ_NO_ERROR   61
IRQ_NO_ERROR   62
IRQ_NO_ERROR   63
IRQ_NO_ERROR   64
IRQ_NO_ERROR   65
IRQ_NO_ERROR   66
IRQ_NO_ERROR   67
IRQ_NO_ERROR   68
IRQ_NO_ERROR   69
IRQ_NO_ERROR   70
IRQ_NO_ERROR   71
IRQ_NO_ERROR   72
IRQ_NO_ERROR   73
IRQ_NO_ERROR   74
IRQ_NO_ERROR   75
IRQ_NO_ERROR   76
IRQ_NO_ERROR   77
IRQ_NO_ERROR   78
IRQ_NO_ERROR   79
IRQ_NO_ERROR   80
IRQ_NO_ERROR   81
IRQ_NO_ERROR   82
IRQ_NO_ERROR   83
IRQ_NO_ERROR   84
IRQ_NO_ERROR   85
IRQ_NO_ERROR   86
IRQ_NO_ERROR   87
IRQ_NO_ERROR   88
IRQ_NO_ERROR   89
IRQ_NO_ERROR   90
IRQ_NO_ERROR   91
IRQ_NO_ERROR   92
IRQ_NO_ERROR   93
IRQ_NO_ERROR   94
IRQ_NO_ERROR   95
IRQ_NO_ERROR   96
IRQ_NO_ERROR   97
IRQ_NO_ERROR   98
IRQ_NO_ERROR   99
IRQ_NO_ERROR   100
IRQ_NO_ERROR   101
IRQ_NO_ERROR   102
IRQ_NO_ERROR   103
IRQ_NO_ERROR   104
IRQ_NO_ERROR   105
IRQ_NO_ERROR   106
IRQ_NO_ERROR   107
IRQ_NO_ERROR   108
IRQ_NO_ERROR   109
IRQ_NO_ERROR   110
IRQ_NO_ERROR   111
IRQ_NO_ERROR   112
IRQ_NO_ERROR   113
IRQ_NO_ERROR   114
IRQ_NO_ERROR   115
IRQ_NO_ERROR   116
IRQ_NO_ERROR   117
IRQ_NO_ERROR   118
IRQ_NO_ERROR   119
IRQ_NO_ERROR   120
IRQ_NO_ERROR   121
IRQ_NO_ERROR   122
IRQ_NO_ERROR   123
IRQ_NO_ERROR   124
IRQ_NO_ERROR   125
IRQ_NO_ERROR   126
IRQ_NO_ERROR   127
IRQ_NO_ERROR   128
IRQ_NO_ERROR   129
IRQ_NO_ERROR   130
IRQ_NO_ERROR   131
IRQ_NO_ERROR   132
IRQ_NO_ERROR   133
IRQ_NO_ERROR   134
IRQ_NO_ERROR   135
IRQ_NO_ERROR   136
IRQ_NO_ERROR   137
IRQ_NO_ERROR   138
IRQ_NO_ERROR   139
IRQ_NO_ERROR   140
IRQ_NO_ERROR   141
IRQ_NO_ERROR   142
IRQ_NO_ERROR   143
IRQ_NO_ERROR   144
IRQ_NO_ERROR   145
IRQ_NO_ERROR   146
IRQ_NO_ERROR   147
IRQ_NO_ERROR   148
IRQ_NO_ERROR   149
IRQ_NO_ERROR   150
IRQ_NO_ERROR   151
IRQ_NO_ERROR   152
IRQ_NO_ERROR   153
IRQ_NO_ERROR   154
IRQ_NO_ERROR   155
IRQ_NO_ERROR   156
IRQ_NO_ERROR   157
IRQ_NO_ERROR   158
IRQ_NO_ERROR   159
IRQ_NO_ERROR   160
IRQ_NO_ERROR   161
IRQ_NO_ERROR   162
IRQ_NO_ERROR   163
IRQ_NO_ERROR   164
IRQ_NO_ERROR   165
IRQ_NO_ERROR   166
IRQ_NO_ERROR   167
IRQ_NO_ERROR   168
IRQ_NO_ERROR   169
IRQ_NO_ERROR   170
IRQ_NO_ERROR   171
IRQ_NO_ERROR   172
IRQ_NO_ERROR   173
IRQ_NO_ERROR   174
IRQ_NO_ERROR   175
IRQ_NO_ERROR   176
IRQ_NO_ERROR   177
IRQ_NO_ERROR   178
IRQ_NO_ERROR   179
IRQ_NO_ERROR   180
IRQ_NO_ERROR   181
IRQ_NO_ERROR   182
IRQ_NO_ERROR   183
IRQ_NO_ERROR   184
IRQ_NO_ERROR   185
IRQ_NO_ERROR   186
IRQ_NO_ERROR   187
IRQ_NO_ERROR   188
IRQ_NO_ERROR   189
IRQ_NO_ERROR   190
IRQ_NO_ERROR   191
IRQ_NO_ERROR   192
IRQ_NO_ERROR   193
IRQ_NO_ERROR   194
IRQ_NO_ERROR   195
IRQ_NO_ERROR   196
IRQ_NO_ERROR   197
IRQ_NO_ERROR   198
IRQ_NO_ERROR   199
IRQ_NO_ERROR   200
IRQ_NO_ERROR   201
IRQ_NO_ERROR   202
IRQ_NO_ERROR   203
IRQ_NO_ERROR   204
IRQ_NO_ERROR   205
IRQ_NO_ERROR   206
IRQ_NO_ERROR   207
IRQ_NO_ERROR   208
IRQ_NO_ERROR   209
IRQ_NO_ERROR   210
IRQ_NO_ERROR   211
IRQ_NO_ERROR   212
IRQ_NO_ERROR   213
IRQ_NO_ERROR   214
IRQ_NO_ERROR   215
IRQ_NO_ERROR   216
IRQ_NO_ERROR   217
IRQ_NO_ERROR   218
IRQ_NO_ERROR   219
IRQ_NO_ERROR   220
IRQ_NO_ERROR   221
IRQ_NO_ERROR   222
IRQ_NO_ERROR   223
IRQ_NO_ERROR   224
IRQ_NO_ERROR   225
IRQ_NO_ERROR   226
IRQ_NO_ERROR   227
IRQ_NO_ERROR   228
IRQ_NO_ERROR   229
IRQ_NO_ERROR   230
IRQ_NO_ERROR   231
IRQ_NO_ERROR   232
IRQ_NO_ERROR   233
IRQ_NO_ERROR   234
IRQ_NO_ERROR   235
IRQ_NO_ERROR   236
IRQ_NO_ERROR   237
IRQ_NO_ERROR   238
IRQ_NO_ERROR   239
IRQ_NO_ERROR   240
IRQ_NO_ERROR   241
IRQ_NO_ERROR   242
IRQ_NO_ERROR   243
IRQ_NO_ERROR   244
IRQ_NO_ERROR   245
IRQ_NO_ERROR   246
IRQ_NO_ERROR   247
IRQ_NO_ERROR   248
IRQ_NO_ERROR   249
IRQ_NO_ERROR   250
IRQ_NO_ERROR   251
IRQ_NO_ERROR   252
IRQ_NO_ERROR   253
IRQ_NO_ERROR   254

    .p2align 4
arch_irq_common:
    testb $3, 24(%rsp)
    jz 7f
    swapgs
7:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    movq %rsp, %rdi
    andq $-16, %rsp
    call x86_irq_dispatch
    movq %r12, %rsp

    testb $3, 144(%rsp)
    jz 8f
    swapgs
8:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

#ifdef __ELF__
    .section .rdata,"a",@progbits
#else
    .section .rdata,"dr"
#endif
    .p2align 3
    .globl arch_irq_stub_table
arch_irq_stub_table:
    .quad arch_irq_33
    .quad arch_irq_34
    .quad arch_irq_35
    .quad arch_irq_36
    .quad arch_irq_37
    .quad arch_irq_38
    .quad arch_irq_39
    .quad arch_irq_40
    .quad arch_irq_41
    .quad arch_irq_42
    .quad arch_irq_43
    .quad arch_irq_44
    .quad arch_irq_45
    .quad arch_irq_46
    .quad arch_irq_47
    .quad arch_irq_48
    .quad arch_irq_49
    .quad arch_irq_50
    .quad arch_irq_51
    .quad arch_irq_52
    .quad arch_irq_53
    .quad arch_irq_54
    .quad arch_irq_55
    .quad arch_irq_56
    .quad arch_irq_57
    .quad arch_irq_58
    .quad arch_irq_59
    .quad arch_irq_60
    .quad arch_irq_61
    .quad arch_irq_62
    .quad arch_irq_63
    .quad arch_irq_64
    .quad arch_irq_65
    .quad arch_irq_66
    .quad arch_irq_67
    .quad arch_irq_68
    .quad arch_irq_69
    .quad arch_irq_70
    .quad arch_irq_71
    .quad arch_irq_72
    .quad arch_irq_73
    .quad arch_irq_74
    .quad arch_irq_75
    .quad arch_irq_76
    .quad arch_irq_77
    .quad arch_irq_78
    .quad arch_irq_79
    .quad arch_irq_80
    .quad arch_irq_81
    .quad arch_irq_82
    .quad arch_irq_83
    .quad arch_irq_84
    .quad arch_irq_85
    .quad arch_irq_86
    .quad arch_irq_87
    .quad arch_irq_88
    .quad arch_irq_89
    .quad arch_irq_90
    .quad arch_irq_91
    .quad arch_irq_92
    .quad arch_irq_93
    .quad arch_irq_94
    .quad arch_irq_95
    .quad arch_irq_96
    .quad arch_irq_97
    .quad arch_irq_98
    .quad arch_irq_99
    .quad arch_irq_100
    .quad arch_irq_101
    .quad arch_irq_102
    .quad arch_irq_103
    .quad arch_irq_104
    .quad arch_irq_105
    .quad arch_irq_106
    .quad arch_irq_107
    .quad arch_irq_108
    .quad arch_irq_109
    .quad arch_irq_110
    .quad arch_irq_111
    .quad arch_irq_112
    .quad arch_irq_113
    .quad arch_irq_114
    .quad arch_irq_115
    .quad arch_irq_116
    .quad arch_irq_117
    .quad arch_irq_118
    .quad arch_irq_119
    .quad arch_irq_120
    .quad arch_irq_121
    .quad arch_irq_122
    .quad arch_irq_123
    .quad arch_irq_124
    .quad arch_irq_125
    .quad arch_irq_126
    .quad arch_irq_127
    .quad arch_irq_128
    .quad arch_irq_129
    .quad arch_irq_130
    .quad arch_irq_131
    .quad arch_irq_132
    .quad arch_irq_133
    .quad arch_irq_134
    .quad arch_irq_135
    .quad arch_irq_136
    .quad arch_irq_137
    .quad arch_irq_138
    .quad arch_irq_139
    .quad arch_irq_140
    .quad arch_irq_141
    .quad arch_irq_142
    .quad arch_irq_143
    .quad arch_irq_144
    .quad arch_irq_145
    .quad arch_irq_146
    .quad arch_irq_147
    .quad arch_irq_148
    .quad arch_irq_149
    .quad arch_irq_150
    .quad arch_irq_151
    .quad arch_irq_152
    .quad arch_irq_153
    .quad arch_irq_154
    .quad arch_irq_155
    .quad arch_irq_156
    .quad arch_irq_157
    .quad arch_irq_158
    .quad arch_irq_159
    .quad arch_irq_160
    .quad arch_irq_161
    .quad arch_irq_162
    .quad arch_irq_163
    .quad arch_irq_164
    .quad arch_irq_165
    .quad arch_irq_166
    .quad arch_irq_167
    .quad arch_irq_168
    .quad arch_irq_169
    .quad arch_irq_170
    .quad arch_irq_171
    .quad arch_irq_172
    .quad arch_irq_173
    .quad arch_irq_174
    .quad arch_irq_175
    .quad arch_irq_176
    .quad arch_irq_177
    .quad arch_irq_178
    .quad arch_irq_179
    .quad arch_irq_180
    .quad arch_irq_181
    .quad arch_irq_182
    .quad arch_irq_183
    .quad arch_irq_184
    .quad arch_irq_185
    .quad arch_irq_186
    .quad arch_irq_187
    .quad arch_irq_188
    .quad arch_irq_189
    .quad arch_irq_190
    .quad arch_irq_191
    .quad arch_irq_192
    .quad arch_irq_193
    .quad arch_irq_194
    .quad arch_irq_195
    .quad arch_irq_196
    .quad arch_irq_197
    .quad arch_irq_198
    .quad arch_irq_199
    .quad arch_irq_200
    .quad arch_irq_201
    .quad arch_irq_202
    .quad arch_irq_203
    .quad arch_irq_204
    .quad arch_irq_205
    .quad arch_irq_206
    .quad arch_irq_207
    .quad arch_irq_208
    .quad arch_irq_209
    .quad arch_irq_210
    .quad arch_irq_211
    .quad arch_irq_212
    .quad arch_irq_213
    .quad arch_irq_214
    .quad arch_irq_215
    .quad arch_irq_216
    .quad arch_irq_217
    .quad arch_irq_218
    .quad arch_irq_219
    .quad arch_irq_220
    .quad arch_irq_221
    .quad arch_irq_222
    .quad arch_irq_223
    .quad arch_irq_224
    .quad arch_irq_225
    .quad arch_irq_226
    .quad arch_irq_227
    .quad arch_irq_228
    .quad arch_irq_229
    .quad arch_irq_230
    .quad arch_irq_231
    .quad arch_irq_232
    .quad arch_irq_233
    .quad arch_irq_234
    .quad arch_irq_235
    .quad arch_irq_236
    .quad arch_irq_237
    .quad arch_irq_238
    .quad arch_irq_239
    .quad arch_irq_240
    .quad arch_irq_241
    .quad arch_irq_242
    .quad arch_irq_243
    .quad arch_irq_244
    .quad arch_irq_245
    .quad arch_irq_246
    .quad arch_irq_247
    .quad arch_irq_248
    .quad arch_irq_249
    .quad arch_irq_250
    .quad arch_irq_251
    .quad arch_irq_252
    .quad arch_irq_253
    .quad arch_irq_254
