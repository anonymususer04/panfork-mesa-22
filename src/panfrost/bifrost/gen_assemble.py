#!/usr/bin/env python3

from bifrost_isa import *
import sys
import itertools
import collections

instructions = parse_instructions(sys.argv[1])
ir_instructions = partition_mnemonics(instructions)
modifier_lists = order_modifiers(ir_instructions)

def yacc_rule(name, options):
    return (name + ":\n  " +
            "| ".join([
                name +
                (" { " + instr + " }\n" if instr else "\n")
                for name, instr in options
            ]) + ";\n")
i = 0

# TODO: Use a dict or something for tokens
tokens = []
yacc = ""
#types = collections.defaultdict([])

SWIZZLES = {"lane", "lanes", "replicate", "swz", "widen", "swap"}

SRC_MODS = {"abs": "abs", "sign": "abs",
            "neg": "neg", "not": "neg"}

# urgh
# only want ones that get used anyway...
# and needs special handing below...

for s in SRC_MODS:
    continue
    for i in range(4):
        modifier_lists[s + str(i)] = modifier_lists[s]

# TODO: normalise mod names...
for mod in modifier_lists:
    mods = modifier_lists[mod]

    if mod == "bytes": mod = "bytes2"

    if mod in SWIZZLES:
        continue

    if mod in SRC_MODS:
        mod_field = "dest[0]." + SRC_MODS[mod]
    elif mod[:-1] in SRC_MODS:
        mod_field = "src[" + mod[-1] + "]." + SRC_MODS[mod[:-1]]
    else:
        mod_field = mod

    def set_mod(value):
        return "instr->{} = {};".format(mod_field, value)

    if len(mods) > 2:
        options = []
        for option in mods:
            if option == "reserved":
                continue
            elif option == "none":
                # This is a GNU extension...
                tok_name = "%empty"
            else:
                tok_name = "T_MOD_" + option.upper()
                # TODO: hack...
                if ("." + option, tok_name) not in tokens:
                    tokens.append(("." + option, tok_name))

            bi_name = "BI_" + mod.upper() + "_" + option.upper()

            options.append((tok_name, set_mod(bi_name)))
    else:
        tok_name = "T_MOD_" + mod.upper()

        options = [("%empty", set_mod("false")),
                   (tok_name, set_mod("true"))]

        if ("." + mod, tok_name) not in tokens:
            tokens.append(("." + mod, tok_name))

    yacc += yacc_rule("mod_" + mod, options)

def mod_normalize(mod):
    if mod in SWIZZLES:
        return "swizzle"
    elif mod[:-1] in SWIZZLES:
        return "swizzle" + mod[-1]
    else:
        return mod

def mod_name(mod):
    return "mod_" + mod_normalize(mod)

# Group instructions by the sources they take
def group_instructions(instructions):
    grouped = {}

    for op in instructions:
        ins = instructions[op]
        num_srcs = ins["srcs"]

        src_mods = (mod_normalize(m) for m in ins["modifiers"]
                    if m[-1] in "0123" and m != "bytes2")

        # TODO: don't sort???
        key = (num_srcs, tuple(sorted(ins["immediates"])),
               tuple(sorted(src_mods)), ins["staging"] != "")

        if key in grouped:
            grouped[key].append(op)
        else:
            grouped[key] = [op]

    return grouped.values()

def op_cmd(op):
    op_name = op.replace(".", "_").upper()
    tok_name = "T_OP_{}".format(op_name)

    if (op, tok_name) not in tokens:
        tokens.append((op, tok_name))

    ins = ir_instructions[op]

    mods = [mod_name(m) for m in ins["modifiers"]
            if m[-1] not in "0123" or m == "bytes2"]

    return (" ".join([tok_name, *mods]),
            "instr->op = BI_OPCODE_{};".format(op_name))

ops_grouped = group_instructions(ir_instructions)

ins_rule = []

for group_num, group in enumerate(ops_grouped):
    group_name = "ins_set_{}".format(group_num)

    yacc += yacc_rule(group_name, map(op_cmd, group))

    ins = ir_instructions[group[0]]

    num_srcs = ins["srcs"]

    srcs = [["src_reg"] for s in range(num_srcs)]
    for mod in ins["modifiers"]:
        num = mod[-1]
        if num in "0123" and mod != "bytes2":
            srcs[int(num)].append(mod_name(mod[:-1]))

    name = (group_name + " " +
            " ',' ".join([
                "dst_reg",
                *[" ".join(x) for x in srcs],
                *([" ".join([
                    "imm_" + i for i in ins["immediates"]
                ])] if ins["immediates"] else []),
                # TODO: immediates: ", {}:%u" e.g. attribute_index:0
                # TODO: if staging, srcs should be offset by one
                *(["staging_reg"] if ins["staging"] else []),
            ]))

    ins_rule.append((name, ""))

yacc += yacc_rule("instr", ins_rule)

tokens += [
    ("osrb", "T_OSRB"),
    ("eos", "T_EOS"),
    ("nbb", "T_NBB"),
    ("br_pc", "T_BR_PC"),
    ("r_uncond", "T_R_UNCOND"),
    ("bb", "T_BB"),
    ("we", "T_WE"),
    ("inf_suppress", "T_INF_SUPPRESS"),
    ("nan_suppress", "T_NAN_SUPPRESS"),
    ("ftz_dx11", "T_FTZ_DX11"),
    ("ftz_hsa", "T_FTZ_HSA"),
    ("ftz_au", "T_FTZ_AU"),
    ("fpe_ts", "T_FPE_TS"),
    ("fpe_pd", "T_FPE_PD"),
    ("fpe_psqr", "T_FPS_PSQR"),
    ("vary", "T_VARYING"),
    ("attr", "T_ATTRIBUTE"),
    ("tex", "T_TEX"),
    ("vartex", "T_VARTEX"),
    ("load", "T_LOAD"),
    ("store", "T_STORE"),
    ("atomic", "T_ATOMIC"),
    ("barrier", "T_BARRIER"),
    ("blend", "T_BLEND"),
    ("tile", "T_TILE"),
    ("z_stencil", "T_Z_STENCIL"),
    ("atest", "T_ATEST"),
    ("job", "T_JOB"),
    ("64", "T_64BIT"),
    ("td", "T_TD"),
    ("ncph", "T_NCPH"),
    ("next_", "T_NEXT"),

    ("foo", "imm_shift"),
    ("foo2", "imm_attribute_index"),
    ("foo3", "imm_texture_index"),
    ("foo4", "imm_index"),
    ("foo5", "imm_sampler_index"),
    ("foo6", "imm_varying_index"),
    ("foo7", "imm_fill"),
]

for tok in "T_CLAUSE", "T_DEPSLOT", "T_REGISTER", "T_T0_T1", "T_ZERO_T", "T_FAU":
    tokens.append((None, tok))

yacc += """

tuple: '*' instr '+' instr { } ;

// TODO: Use bi_register and bi_null()
dst_reg:
  T_REGISTER ':' T_T0_T1
| T_T0_T1
;

src_reg:
  T_REGISTER
| T_ZERO_T
| T_FAU
| T_T0_T1
;

// Ugggggghhh
mod_swizzle:
;

// UGGGGGGGGGGGGGGGGhhhhhhhhhhhhhhhh
staging_reg:
  '@' T_REGISTER
;

tuples:
  tuple tuples
| tuple
;

clause_staging:
  %empty
| T_OSRB
;

clause_flow:
  T_EOS             { $$ = BIFROST_FLOW_END; }
| T_NBB T_BR_PC     { $$ = BIFROST_FLOW_NBTB_PC; }
| T_NBB T_R_UNCOND  { $$ = BIFROST_FLOW_NBTB_UNCONDITIONAL; }
| T_NBB             { $$ = BIFROST_FLOW_NBTB; }
| T_BB T_R_UNCOND   { $$ = BIFROST_FLOW_BTB_UNCONDITIONAL; }
| T_BB              { $$ = BIFROST_FLOW_BTB_NONE; }
| T_WE T_R_UNCOND   { $$ = BIFROST_FLOW_WE_UNCONDITIONAL; }
| T_WE              { $$ = BIFROST_FLOW_WE; }
;

clause_inf_suppress:
  %empty            { $$ = false; }
| T_INF_SUPPRESS    { $$ = true; }
;

clause_nan_suppress:
  %empty            { $$ = false; }
| T_NAN_SUPPRESS    { $$ = true; }
;

clause_ftz:
  %empty            { $$ = BIFROST_FTZ_DISABLE; }
| T_FTZ_DX11        { $$ = BIFROST_FTZ_DX11; }
| T_FTZ_HSA         { $$ = BIFROST_FTZ_ALWAYS; }
| T_FTZ_AU          { $$ = BIFROST_FTZ_ABRUPT; }
;

clause_fpe:
  %empty            { $$ = BIFROST_EXCEPTIONS_ENABLED; }
| T_FPE_TS          { $$ = BIFROST_EXCEPTIONS_DISABLED; }
| T_FPE_PD          { $$ = BIFROST_EXCEPTIONS_PRECISE_DIVISION; }
| T_FPS_PSQR        { $$ = BIFROST_EXCEPTIONS_PRECISE_SQRT; }
;

clause_message:
  %empty            { $$ = BIFROST_MESSAGE_NONE; }
| T_VARYING         { $$ = BIFROST_MESSAGE_VARYING; }
| T_ATTRIBUTE       { $$ = BIFROST_MESSAGE_ATTRIBUTE; }
| T_TEX             { $$ = BIFROST_MESSAGE_TEX; }
| T_VARTEX          { $$ = BIFROST_MESSAGE_VARTEX; }
| T_LOAD            { $$ = BIFROST_MESSAGE_LOAD; }
| T_STORE           { $$ = BIFROST_MESSAGE_STORE; }
| T_ATOMIC          { $$ = BIFROST_MESSAGE_ATOMIC; }
| T_BARRIER         { $$ = BIFROST_MESSAGE_BARRIER; }
| T_BLEND           { $$ = BIFROST_MESSAGE_BLEND; }
| T_TILE            { $$ = BIFROST_MESSAGE_TILE; }
| T_Z_STENCIL       { $$ = BIFROST_MESSAGE_Z_STENCIL; }
| T_ATEST           { $$ = BIFROST_MESSAGE_ATEST; }
| T_JOB             { $$ = BIFROST_MESSAGE_JOB; }
| T_64BIT           { $$ = BIFROST_MESSAGE_64BIT; }
;

clause_next_message:
  %empty                { $$ = 0; }
| T_NEXT clause_message { $$ = $1; }
;

clause_td:
  %empty            { $$ = false; }
| T_TD              { $$ = true; }
;

clause_prefetch:
  %empty            { $$ = false; }
| T_NCPH            { $$ = true; }
;

clause_dep_wait:
;

clause_header:
  T_DEPSLOT clause_staging clause_flow
    clause_inf_suppress clause_nan_suppress clause_ftz clause_fpe
    clause_message clause_td clause_prefetch clause_next_message
    clause_dep_wait {
      struct bifrost_header header = {
          .dependency_slot = $T_DEPSLOT,
          .staging_barrier = $clause_staging,
          .flow_control = $clause_flow,
          .suppress_inf = $clause_inf_suppress,
          .suppress_nan = $clause_nan_suppress,
          .flush_to_zero = $clause_ftz,
          .float_exceptions = $clause_fpe,
          .message_type = $clause_message,
          .terminate_discarded_threads = $clause_td,
          .next_clause_prefetch = $clause_prefetch,
          .next_message_type = $clause_next_message,
          .dependency_wait = $clause_dep_wait,
      };
}
;

clause_num:
;

clause:
  T_CLAUSE clause_num ':' clause_header '{' tuples '}'
;

"""

pre = COPYRIGHT + """
%define api.value.type {int}
%define parse.error detailed

%{
#include <stdio.h>

#include "compiler.h"

bi_instr *instr;

int yydebug;

int bi_yyget_lineno(void);

static void yyerror(const char *error)
{
	fprintf(stderr, "error at line %d: %s\\n", bi_yyget_lineno(), error);
}

void bi_parse(/*struct ir3_shader_variant *v,
              struct ir3_kernel_info *k, */FILE *f);

void bi_yyset_input(FILE *f);
void bi_yyset_lineno(int line);

int yyparse(void);

void bi_parse(/*struct ir3_shader_variant *v,
              struct ir3_kernel_info *k, */FILE *f)
{
        bi_yyset_lineno(1);
        bi_yyset_input(f);
//#ifdef YYDEBUG
        yydebug = 1;
//#endif
bi_instr a_instr = {0};
instr = &a_instr;
    yyparse();
//        info = k;
//        variant = v;
//        if (yyparse() || !resolve_labels()) {
//                ir3_destroy(variant->ir);
//                variant->ir = NULL;
//        }
//        ralloc_free(labels);
//        ralloc_free(ir3_parser_dead_ctx);
//        return variant->ir;
}

extern int yylex(void);

%}

"""

# TODO: Do we need types?

mid = """
%%

shader:
  %empty
| shader clause
;

"""

lex_pre = COPYRIGHT + """
%{
#include <stdlib.h>
#include "bi_parser.h"
//#include "asm.h"

#define YY_NO_INPUT
#define YY_NO_UNPUT

#define TOKEN(t) t
//(yylval.tok = t)
extern YYSTYPE yylval;

void bi_yyset_input(FILE *f);

void bi_yyset_input(FILE *f)
{
	YY_FLUSH_BUFFER;
	bi_yyin = f;
}

static int parse_reg(const char *str)
{
	int num = 0;
//	if (str[0] == 'h') {
//		str++;
//		num++;
//	}
	str++;
	num = strtol(str, (char **)&str, 10);
//	switch (str[1]) {
//	case 'x': num += 0; break;
//	case 'y': num += 2; break;
//	case 'z': num += 4; break;
//	case 'w': num += 6; break;
//	default: assert(0); break;
//	}
	return num;
}

%}

%option noyywrap
%option prefix="bi_yy"

%%
"\\n"                              yylineno++;
[ \\t]+                             ; /* ignore whitespace */
";"[^\\n]*"\\n"                     yylineno++; /* ignore comments */

"r"[0-9]+ bi_yylval = parse_reg(yytext); return T_REGISTER;

"t"[01] return TOKEN(T_T0_T1);
"#0"|"t" return TOKEN(T_ZERO_T);
clause_[0-9]+ return TOKEN(T_CLAUSE);
"ds("[0-9]"u)" return TOKEN(T_DEPSLOT);

"""
# But we only need hex??
lex_foo = """
 /* Scan an integer.  */
[0-9]+   {
  errno = 0;
  long n = strtol (yytext, NULL, 10);
  if (! (INT_MIN <= n && n <= INT_MAX && errno != ERANGE))
    yyerror (yylloc, nerrs, "integer is out of range");
  yylval->TOK_NUM = (int) n;
  return TOK_NUM;
}
"""

lex_post = "\n"
for tok in ":", "{", "}", "*", "+", ",", "@":
    # TODO: Remove use of f-strings
    lex_post += f"\"{tok}\" return '{tok}';\n"

lex_post += """

.                                 fprintf(stderr, "error at line %d: Unknown token: %s\\n", bi_yyget_lineno(), yytext); yyterminate();

%%

"""

# TODO: Use some templating library instead of str concat

# <tok> if using multiple types...
with open(sys.argv[2], "w") as f:
    f.write(pre)
    f.write("\n".join(map(lambda t: "%token " + t[1], tokens)))
    f.write(mid)
    f.write(yacc)

with open(sys.argv[3], "w") as f:
    f.write(lex_pre)
    f.write("\n".join(['"{}" return TOKEN({});'.format(t[0], t[1]) for t in tokens
                       if t[0]]))
    f.write(lex_post)
