/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
/*
 * dwoff - offline tool extracting method B offsets from a CUBRID build's DWARF
 *
 * The offset tables come from the binary rather than by hand.  Only the generated
 * table is deployed, so there is no runtime cost or risk on a production host.
 *
 * Design
 *   1) CU index - abbreviation table, version and address size differ per CU, so the
 *      owning CU must be known to decode a DIE
 *   2) symbol -> variable DIE (DW_AT_name or DW_AT_linkage_name) -> DW_AT_type
 *   3) a type offset pointing into another CU is resolved by rewalking that CU alone,
 *      passing through typedef/const/volatile
 *   4) members are searched recursively by name, accumulating the offset through
 *      nested structs
 *
 * Safety rules (L1/L2)
 *   L1 structure: if a CU walk does not land exactly on cu_end, or an unknown abbrev
 *                 or form appears, that CU's results are discarded entirely
 *   L2 sanity: sizeof between 1B and 1MB, member offset < sizeof, offset aligned
 *   Several candidates: all variable DIEs of one name are kept as candidates and only
 *                 the first that resolves to a struct or base type is adopted
 *
 * Build: gcc -O2 -std=gnu99 -o dwoff dwoff.c        (no external libraries)
 * Usage: ./dwoff <libcubrid.so> [--emit-c | --emit-py]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

static double now_ms(void){ struct timeval t; gettimeofday(&t,0); return t.tv_sec*1000.0+t.tv_usec/1000.0; }
/* The input .so is not trusted: cub_top runs this same engine on whatever process
   is named cub_server (see sym_lookup there), and the two copies are kept in step.  Every read is bounded
   by the end of the area being parsed: DW_END is set by the caller, and a read that
   would cross it stops and raises DW_BAD, which the caller turns into a failure. */
static const unsigned char *DW_END; static int DW_BAD;
static uint64_t uleb(const unsigned char**p){ uint64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(uint64_t)(b&0x7f)<<s; s+=7; }while(b&0x80); return r; }
static int64_t sleb(const unsigned char**p){ int64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(int64_t)(b&0x7f)<<s; s+=7; }while(b&0x80);
    if(s<64&&(b&0x40)) r|=-((int64_t)1<<s);
    return r; }

/* DWARF tags and attributes */
#define T_struct   0x13
#define T_class    0x02
#define T_union    0x17
#define T_member   0x0d
#define T_var      0x34
#define T_typedef  0x16
#define T_const    0x26
#define T_volatile 0x35
#define T_base     0x24
#define A_name     0x03
#define A_bsize    0x0b
#define A_dml      0x38
#define A_type     0x49
#define A_loc      0x02
#define A_decl     0x3c
#define A_lnk      0x6e
#define A_mips_lnk 0x2007

static const unsigned char *DI,*AB,*DS; static size_t DI_SZ, AB_SZ, DS_SZ;

typedef struct { unsigned long start,first,end; uint64_t ab_off; int ver,addr_sz,off_sz; } cu_t;
static cu_t *CU; static int NCU;

typedef struct { uint64_t at,form; int64_t ic; } af_t;
typedef struct { uint64_t code,tag; int kids,n; af_t a[160]; } ab_t;
static ab_t TAB[32768]; static int NTAB;
static long G_desync=0;

static int load_abbrev(uint64_t off){
    NTAB=0;
    if(off>=AB_SZ) return -1;
    const unsigned char*a=AB+off;
    DW_END=AB+AB_SZ; DW_BAD=0;
    while(NTAB<32768){
        uint64_t code=uleb(&a); if(DW_BAD) return -1; if(!code) return 0;
        ab_t*t=&TAB[NTAB++]; t->code=code; t->tag=uleb(&a);
        if(DW_BAD||a>=DW_END) return -1;
        t->kids=*a++; t->n=0;
        for(;;){
            uint64_t at=uleb(&a), fm=uleb(&a); int64_t ic=0;
            if(fm==0x21) ic=sleb(&a);
            if(DW_BAD) return -1;
            if(!at&&!fm) break;
            if(t->n>=160) return -1;
            t->a[t->n].at=at; t->a[t->n].form=fm; t->a[t->n].ic=ic; t->n++;
        }
    }
    return -1;
}
static ab_t* ab_find(uint64_t code){
    for(int i=0;i<NTAB;i++) if(TAB[i].code==code) return &TAB[i];
    return NULL;
}
/* Consume one form; an unknown form fails at once rather than desyncing silently */
static int form_skip(const unsigned char**p,const unsigned char*lim,uint64_t form,int64_t ic,
                     const cu_t*cu,uint64_t*uv,const char**sv){
    const unsigned char*q=*p; uint64_t v=0;
    if(q>=lim&&form!=0x19&&form!=0x21) return -1;
    /* Room for n more bytes, checked before they are read */
    #define NEED(n_) do{ if((size_t)(lim-q)<(size_t)(n_)) return -1; }while(0)
    #define BLOCK(n_) do{ uint64_t bn_=(n_); if(DW_BAD||bn_>(uint64_t)(lim-q)) return -1; \
        if(bn_&&*q==0x23){ const unsigned char*r=q+1; v=uleb(&r); } q+=bn_; }while(0)
    switch(form){
    case 0x01: NEED(cu->addr_sz); v=cu->addr_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->addr_sz; break;
    case 0x03: NEED(2); v=*(uint16_t*)q; q+=2; NEED(v); q+=v; break;     /* block2: value = length */
    case 0x04: NEED(4); v=*(uint32_t*)q; q+=4; NEED(v); q+=v; break;     /* block4: value = length */
    case 0x05: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x06: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x07: NEED(8); v=*(uint64_t*)q; q+=8; break;
    case 0x08: { const unsigned char*z=memchr(q,0,(size_t)(lim-q));
                 if(!z) return -1;
                 if(sv) *sv=(const char*)q;
                 q=z+1; } break;
    case 0x09: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x0a: { NEED(1); uint64_t n=*q++; BLOCK(n); } break;
    case 0x0b: NEED(1); v=*q++; break;
    case 0x0c: NEED(1); v=*q++; break;
    case 0x0d: v=(uint64_t)sleb(&q); break;
    case 0x0e: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz;
               if(sv&&DS){ if(v>=DS_SZ||!memchr(DS+v,0,DS_SZ-(size_t)v)) return -1;
                           *sv=(const char*)(DS+v); }
               break;
    case 0x0f: v=uleb(&q); break;
    case 0x10: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break; /* Section-relative */
    case 0x11: NEED(1); v=cu->start+*q; q+=1; break;                                      /* CU-relative from here */
    case 0x12: NEED(2); v=cu->start+*(uint16_t*)q; q+=2; break;
    case 0x13: NEED(4); v=cu->start+*(uint32_t*)q; q+=4; break;
    case 0x14: NEED(8); v=cu->start+*(uint64_t*)q; q+=8; break;
    case 0x15: { uint64_t r=uleb(&q); v=cu->start+r; } break;
    case 0x16: { uint64_t f2=uleb(&q); if(DW_BAD||f2==0x16) return -1;   /* indirect to indirect: no end */
                 *p=q; return form_skip(p,lim,f2,0,cu,uv,sv); }
    case 0x17: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x18: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x19: v=1; break;
    case 0x1a: v=uleb(&q); break;
    case 0x1b: v=uleb(&q); break;
    case 0x1c: NEED(4); q+=4; break;
    case 0x1d: NEED(cu->off_sz); q+=cu->off_sz; break;
    case 0x1e: NEED(16); q+=16; break;
    case 0x1f: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x20: NEED(8); q+=8; break;
    case 0x21: v=(uint64_t)ic; break;
    case 0x22: v=uleb(&q); break;
    case 0x23: v=uleb(&q); break;
    case 0x24: NEED(8); q+=8; break;
    case 0x25: NEED(1); v=*q++; break;
    case 0x26: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x27: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x28: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x29: NEED(1); v=*q++; break;
    case 0x2a: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x2b: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x2c: NEED(4); v=*(uint32_t*)q; q+=4; break;
    default: return -1;
    }
    #undef NEED
    #undef BLOCK
    if(DW_BAD||q>lim) return -1;
    if(uv)*uv=v;
    *p=q;
    return 0;
}
/* The debug sections, and what about them this engine cannot read: compressed sections
   (-gz: SHF_COMPRESSED; old .zdebug_*), debug info moved to another file (.gnu_debuglink),
   dwz's shared file (.gnu_debugaltlink), DWARF5 string indices (.debug_str_offsets, strx).
   Sets DI/AB/DS.  0 = read, -1 = section table outside the file, -2 = names outside it. */
enum { DWF_COMPRESSED=1, DWF_ZDEBUG=2, DWF_DEBUGLINK=4, DWF_ALTLINK=8, DWF_STROFFS=16 };
static int G_split=0;                /* DWARF5 skeleton/split units: the DIEs are in .dwo files */
static int dw_sections(const unsigned char*m,size_t fsz,int*flags){
    #define DS_IN(off_,len_) ((uint64_t)(off_) <= (uint64_t)fsz && (uint64_t)(len_) <= (uint64_t)fsz - (uint64_t)(off_))
    const Elf64_Ehdr*eh=(const Elf64_Ehdr*)m; *flags=0;
    if(eh->e_shentsize!=sizeof(Elf64_Shdr) || eh->e_shnum==0 || eh->e_shstrndx>=eh->e_shnum ||
       !DS_IN(eh->e_shoff,(uint64_t)eh->e_shnum*sizeof(Elf64_Shdr))) return -1;
    const Elf64_Shdr*sh=(const Elf64_Shdr*)(m+eh->e_shoff);
    const Elf64_Shdr*ssh=&sh[eh->e_shstrndx];
    if(!DS_IN(ssh->sh_offset,ssh->sh_size)) return -2;
    const char*ss=(const char*)(m+ssh->sh_offset);
    for(int i=0;i<eh->e_shnum;i++){
        if(sh[i].sh_name>=ssh->sh_size || !memchr(ss+sh[i].sh_name,0,ssh->sh_size-sh[i].sh_name)) continue;
        const char*n=ss+sh[i].sh_name;
        if(!strncmp(n,".zdebug_",8)) *flags|=DWF_ZDEBUG;
        if(!strcmp(n,".gnu_debuglink")) *flags|=DWF_DEBUGLINK;
        if(!strcmp(n,".gnu_debugaltlink")) *flags|=DWF_ALTLINK;
        if(!strcmp(n,".debug_str_offsets")) *flags|=DWF_STROFFS;
        if(!DS_IN(sh[i].sh_offset,sh[i].sh_size)) continue;   /* a section outside the file is not used */
        int dbg = !strcmp(n,".debug_info")||!strcmp(n,".debug_abbrev")||!strcmp(n,".debug_str");
        if(dbg && (sh[i].sh_flags&SHF_COMPRESSED)){ *flags|=DWF_COMPRESSED; continue; }
        if(!strcmp(n,".debug_info")){ DI=m+sh[i].sh_offset; DI_SZ=sh[i].sh_size; }
        else if(!strcmp(n,".debug_abbrev")){ AB=m+sh[i].sh_offset; AB_SZ=sh[i].sh_size; }
        else if(!strcmp(n,".debug_str")){    DS=m+sh[i].sh_offset; DS_SZ=sh[i].sh_size; }
    }
    #undef DS_IN
    return 0;
}
static int build_index(void){
    int cap=4096; CU=malloc(sizeof(cu_t)*cap); NCU=0;
    if(!CU) return 0;
    const unsigned char*p=DI,*end=DI+DI_SZ;
    while(p+11<end){
        unsigned long cs=(unsigned long)(p-DI);
        uint32_t len=*(uint32_t*)p; p+=4; int osz=4;
        if(len==0xffffffff){ len=(uint32_t)*(uint64_t*)p; p+=8; osz=8; }
        if(len>(uint64_t)(end-p)) break;
        const unsigned char*ce=p+len;
        if((size_t)(ce-p) < (size_t)(2+2+osz)) break;    /* version, unit type/addr size, abbrev offset */
        uint16_t ver=*(uint16_t*)p; p+=2;
        uint64_t ao; int asz;
        if(ver>=5){ int ut=*p++; asz=*p++; ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz;
                    if(ut==4||ut==5){ p+=8; G_split++; }                   /* skeleton/split: dwo_id */
                    else if(ut==2||ut==6) p+=8+(size_t)osz;                 /* type unit: signature, type offset */
                    if(p>ce) break; }
        else { ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz; asz=*p++; }
        if(asz!=4 && asz!=8) break;                        /* form_skip reads addresses of this size */
        if(NCU==cap){ cu_t *t=realloc(CU,sizeof(cu_t)*(size_t)cap*2);
                      if(!t) break;           /* keep what is indexed so far */
                      CU=t; cap*=2; }
        cu_t*c=&CU[NCU++];
        c->start=cs; c->first=(unsigned long)(p-DI); c->end=(unsigned long)(ce-DI);
        c->ab_off=ao; c->ver=ver; c->addr_sz=asz; c->off_sz=osz;
        p=ce;
    }
    return NCU;
}
static cu_t* cu_of(unsigned long off){
    int lo=0,hi=NCU-1;
    while(lo<=hi){ int m=(lo+hi)/2;
        if(off<CU[m].start) hi=m-1; else if(off>=CU[m].end) lo=m+1; else return &CU[m]; }
    return NULL;
}

/* ---- One DIE ---- */
typedef struct {
    int tag; const char*name; long bsize; unsigned long type; int is_decl; int has_loc;
    int nmem; char mname[128][80]; long moff[128]; unsigned long mtype[128];
} die_t;

/* Walk the CUs to either collect variables matching a name or resolve a DIE at an offset */
typedef struct {
    const char*var;                 /* Variable name to find (mangled or source form) */
    unsigned long cand[32]; int ncand, cand_def[32];
    unsigned long want;             /* DIE offset to resolve */
    die_t out; int found;
    const char*type_name;           /* Find a struct by name */
    int pre;                        /* The one-pass index of every wanted name (prescan) */
} job_t;
/* One pass over .debug_info collects, for every name the table wants, the variable candidates
   and the first struct definition in each CU.  The per-key lookups below then read only the
   CUs that matter; a full pass per key (~55 of them over 100-200MB) took 26-46s. */
#define PRE_MAX 128
#define PRE_PER 256
typedef struct { const char*name; int is_type, next, last_cu;
                 int ncand; unsigned long cand[32]; int cand_def[32];
                 int ntd; unsigned long td[PRE_PER]; long tdsz[PRE_PER]; } pre_t;
static pre_t *PRE=NULL; static int NPRE=0, PRE_HEAD[256];
static unsigned pre_hash(const char*s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h&255; }
static pre_t *pre_find(const char*nm,int is_type){
    if(!PRE||!nm) return NULL;
    for(int i=PRE_HEAD[pre_hash(nm)];i>=0;i=PRE[i].next)
        if(PRE[i].is_type==is_type && !strcmp(PRE[i].name,nm)) return &PRE[i];
    return NULL;
}
static void pre_add(const char*nm,int is_type){
    if(!nm||!PRE||NPRE>=PRE_MAX||pre_find(nm,is_type)) return;
    pre_t*e=&PRE[NPRE]; memset(e,0,sizeof *e); e->name=nm; e->is_type=is_type; e->last_cu=-1;
    unsigned h=pre_hash(nm); e->next=PRE_HEAD[h]; PRE_HEAD[h]=NPRE++;
}

static int scan_cu(cu_t*cu, job_t*J){
    if(load_abbrev(cu->ab_off)<0){ G_desync++; return -1; }
    const unsigned char*p=DI+cu->first, *lim=DI+cu->end;
    DW_END=lim; DW_BAD=0;
    int depth=0, in=-1;
    while(p<lim){
        unsigned long doff=(unsigned long)(p-DI);
        uint64_t code=uleb(&p);
        if(DW_BAD){ G_desync++; return -1; }
        if(!code){ if(depth>0)depth--; if(in>=0&&depth<=in) in=-1; continue; }
        ab_t*t=ab_find(code); if(!t){ G_desync++; return -1; }
        const char*nm=NULL,*lnk=NULL; uint64_t bsz=0,ty=(uint64_t)-1,dml=(uint64_t)-1;
        int is_decl=0,has_loc=0,have_dml=0;
        for(int i=0;i<t->n;i++){
            uint64_t v=0; const char*sv=NULL;
            if(form_skip(&p,lim,t->a[i].form,t->a[i].ic,cu,&v,&sv)<0){ G_desync++; return -1; }
            switch(t->a[i].at){
            case A_name: nm=sv; break;
            case A_lnk: case A_mips_lnk: lnk=sv; break;
            case A_bsize: bsz=v; break;
            case A_type: ty=v; break;
            case A_dml: dml=v; have_dml=1; break;
            case A_loc: has_loc=1; break;
            case A_decl: is_decl=(v!=0); break;
            default: break; }
        }
        if(J->pre){
            if(t->tag==T_var && ty!=(uint64_t)-1){
                pre_t*e=pre_find(nm,0); if(!e) e=pre_find(lnk,0);
                if(e && e->ncand<32){ e->cand[e->ncand]=(unsigned long)ty; e->cand_def[e->ncand]=(has_loc&&!is_decl); e->ncand++; }
            } else if((t->tag==T_struct||t->tag==T_class||t->tag==T_union) && nm && bsz>0){
                pre_t*e=pre_find(nm,1); int ci=(int)(cu-CU);
                if(e && e->last_cu!=ci && e->ntd<PRE_PER){ e->td[e->ntd]=doff; e->tdsz[e->ntd]=(long)bsz; e->ntd++; e->last_cu=ci; }
            }
            if(t->kids) depth++;
            continue;
        }
        if(J->var && t->tag==T_var && ty!=(uint64_t)-1 &&
           ((nm&&!strcmp(nm,J->var))||(lnk&&!strcmp(lnk,J->var)))){
            if(J->ncand<32){ J->cand[J->ncand]=(unsigned long)ty;
                             J->cand_def[J->ncand]=(has_loc&&!is_decl); J->ncand++; }
        }
        if(J->type_name && (t->tag==T_struct||t->tag==T_class||t->tag==T_union) &&
           nm && !strcmp(nm,J->type_name) && bsz>0 && !J->found){
            J->out.tag=t->tag; J->out.name=nm; J->out.bsize=(long)bsz; J->found=1;
            J->out.type=(unsigned long)-1; J->out.nmem=0; in=depth;
        }
        if(J->want && doff==J->want){
            J->out.tag=(int)t->tag; J->out.name=nm; J->out.bsize=(long)bsz;
            J->out.type=(unsigned long)ty; J->out.is_decl=is_decl; J->out.has_loc=has_loc;
            J->out.nmem=0; J->found=1; in=depth;
        }
        else if(in>=0 && t->tag==T_member && depth==in+1 && nm && have_dml && J->out.nmem<128){
            int k=J->out.nmem;
            snprintf(J->out.mname[k],80,"%s",nm);
            J->out.moff[k]=(long)dml; J->out.mtype[k]=(unsigned long)ty; J->out.nmem++;
        }
        if(t->kids) depth++;
    }
    if(p!=lim){ G_desync++; return -1; }      /* L1: must land exactly to be trusted */
    return 0;
}

/* Build the index for these names (vars: variable roots, types: struct names) */
static void prescan(const char*const*vars,int nv,const char*const*types,int nt){
    if(!PRE) PRE=(pre_t*)calloc(PRE_MAX,sizeof(pre_t));
    if(!PRE) return;
    NPRE=0; for(int i=0;i<256;i++) PRE_HEAD[i]=-1;
    for(int i=0;i<nv;i++) pre_add(vars[i],0);
    for(int i=0;i<nt;i++) pre_add(types[i],1);
    job_t J; memset(&J,0,sizeof J); J.pre=1;
    for(int c=0;c<NCU;c++) scan_cu(&CU[c],&J);
}
/* The DIE at an offset, read once: type chains and nested members revisit the same DIEs
   for every key, each visit a scan of a whole CU */
#define MEMO_N 4096
typedef struct { unsigned long off; int rc; die_t d; } memo_t;
static memo_t **MEMO=NULL;
static int die_at(unsigned long off,die_t*out){
    unsigned h=(unsigned)((off*2654435761u)>>4)&(MEMO_N-1);
    if(!MEMO) MEMO=(memo_t**)calloc(MEMO_N,sizeof *MEMO);
    for(unsigned k=0;MEMO && k<MEMO_N;k++){
        memo_t*m=MEMO[(h+k)&(MEMO_N-1)];
        if(!m) break;
        if(m->off==off){ if(m->rc==0) *out=m->d; return m->rc; }
    }
    cu_t*c=cu_of(off); int rc=-1;
    job_t*J=(job_t*)calloc(1,sizeof *J);
    if(c && J){ J->want=off; if(scan_cu(c,J)==0 && J->found){ *out=J->out; rc=0; } }
    if(MEMO){ memo_t*m=(memo_t*)malloc(sizeof *m);
        if(m){ m->off=off; m->rc=rc; if(rc==0) m->d=J->out;
            for(unsigned k=0;k<MEMO_N;k++){ unsigned i=(h+k)&(MEMO_N-1); if(!MEMO[i]){ MEMO[i]=m; m=NULL; break; } }
            free(m); } }
    free(J);
    return rc;
}
static void prescan_free(void){
    free(PRE); PRE=NULL; NPRE=0;
    if(MEMO){ for(int i=0;i<MEMO_N;i++) free(MEMO[i]); free(MEMO); MEMO=NULL; }
}
/* Resolve the type chain through typedef/const/volatile to a struct or base DIE */
static int resolve_type(unsigned long off, die_t*out){
    for(int g=0; g<16; g++){
        job_t J; memset(&J,0,sizeof J);
        if(die_at(off,&J.out)<0) return -1;
        if(J.out.tag==T_struct||J.out.tag==T_class||J.out.tag==T_union||J.out.tag==T_base){
            *out=J.out; return 0;
        }
        if(J.out.type==(unsigned long)-1) return -1;
        off=J.out.type;
    }
    return -1;
}
/* L2 value check */
static int sane_struct(const die_t*d){
    if(d->bsize<=0||d->bsize>(1<<20)) return 0;
    for(int i=0;i<d->nmem;i++)
        if(d->moff[i]<0||d->moff[i]>=d->bsize) return 0;
    return 1;
}
/* Symbol to type DIE; with several candidates, prefer the definition and discard those that fail */
static int resolve_symbol(const char*sym, die_t*out, unsigned long*type_off){
    job_t J; memset(&J,0,sizeof J); J.var=sym;
    pre_t*e=pre_find(sym,0);
    if(e){ J.ncand=e->ncand; memcpy(J.cand,e->cand,sizeof J.cand); memcpy(J.cand_def,e->cand_def,sizeof J.cand_def); }
    else for(int c=0;c<NCU;c++){ scan_cu(&CU[c],&J); }   /* Collect candidates from every CU */
    if(!J.ncand) return -1;
    /* Do not adopt when several DIEs share the name: a DIE picked by name alone is
       not guaranteed to be the symtab symbol (static variables repeat per CU).
       An ambiguous key is left empty; the runtime gate decides in any case. */
    if(J.ncand>1){
        /* Exception 1: an extern global declared in a header has many candidates but one
           definition (has a location, not a declaration) - use it if unique.
           Exception 2: if every candidate resolves to the same (type, sizeof) there
           is only one entity, so take the first.  Otherwise still refuse. */
        int ndef=0, di=-1;
        for(int i=0;i<J.ncand;i++) if(J.cand_def[i]){ ndef++; di=i; }
        if(ndef==1){ J.cand[0]=J.cand[di]; J.cand_def[0]=1; J.ncand=1; }
        else {
            die_t d0; int same=1;
            if(resolve_type(J.cand[0],&d0)<0) return -1;
            for(int i=1;i<J.ncand && same;i++){
                die_t di2;
                if(resolve_type(J.cand[i],&di2)<0 || di2.bsize!=d0.bsize ||
                   !d0.name || !di2.name || strcmp(d0.name,di2.name)) same=0;
            }
            if(!same) return -1;
            J.ncand=1;
        }
    }
    for(int pass=0;pass<2;pass++){                        /* Definitions first, then the rest */
        for(int i=0;i<J.ncand;i++){
            if(pass==0 && !J.cand_def[i]) continue;
            if(pass==1 &&  J.cand_def[i]) continue;
            die_t d;
            if(resolve_type(J.cand[i],&d)<0) continue;     /* Discard invalid candidates */
            if(d.tag!=T_base && !sane_struct(&d)) continue;
            *out=d; if(type_off)*type_off=J.cand[i]; return 0;
        }
    }
    return -1;
}
/* Search member names recursively, nested included, accumulating the offset */
static int find_member(const die_t*d,const char*want,long base,int depth,long*res,char*path,size_t plen){
    if(depth>4) return -1;
    for(int i=0;i<d->nmem;i++){
        if(!strcmp(d->mname[i],want)){ *res=base+d->moff[i];
            snprintf(path,plen,"%s",d->mname[i]); return 0; }
    }
    for(int i=0;i<d->nmem;i++){
        if(d->mtype[i]==(unsigned long)-1) continue;
        die_t sub;
        if(resolve_type(d->mtype[i],&sub)<0) continue;
        if(sub.tag==T_base) continue;
        if(!sane_struct(&sub)) continue;
        char p2[160];
        if(find_member(&sub,want,base+d->moff[i],depth+1,res,p2,sizeof p2)==0){
            snprintf(path,plen,"%.80s.%.160s",d->mname[i],p2); return 0;
        }
    }
    return -1;
}
/* sizeof a struct by name */
static int sizeof_type(const char*name,long*sz,const char**real){
    pre_t*e=pre_find(name,1);
    if(e){ for(int i=0;i<e->ntd;i++) if(e->tdsz[i]>0 && e->tdsz[i]<=(1<<20)){ *sz=e->tdsz[i]; if(real)*real=name; return 0; }
           if(e->ntd<PRE_PER) return -1; }
    job_t J; memset(&J,0,sizeof J); J.type_name=name;
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.bsize<=(1<<20)){
            *sz=J.out.bsize; if(real)*real=name; return 0; }
    }
    return -1;
}

/* Member offset within a struct named directly (nested included), for types with no
   variable root - a BCB array element beyond a pointer.  Definitions only (bsize>0). */
static int member_of_type(const char*name,const char*member,long*off){
    pre_t*e=pre_find(name,1);
    if(e){ for(int i=0;i<e->ntd;i++){
               job_t K; memset(&K,0,sizeof K);
               if(die_at(e->td[i],&K.out)<0) continue;
               if(K.out.bsize>0 && K.out.nmem>0){
                   char path[320];
                   if(find_member(&K.out,member,0,0,off,path,sizeof path)==0) return 0; } }
           if(e->ntd<PRE_PER) return -1; }
    job_t J; memset(&J,0,sizeof J);
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.nmem>0){
            char path[320];
            if(find_member(&J.out,member,0,0,off,path,sizeof path)==0) return 0;
        }
    }
    return -1;
}

/* ---- Extraction targets: the values actually read ----
   sym+member = member offset from a variable root; type alone = sizeof;
   type+member = member offset within that type. */
typedef struct { const char*key,*sym,*member,*type; long expect; } tgt_t;
static tgt_t TG[]={
  {"OFF_PGBUF_NBUF",    "pgbuf_Pool","num_buffers",          NULL,  0},
  {"OFF_XCACHE_ENTRIES","xcache_Global","entry_count",       NULL,440},
  {"OFF_XCACHE_USAGE",  "xcache_Global","memory_usage_cache",NULL,484},
  {"OFF_XCACHE_CLONE",  "xcache_Global","memory_usage_clone",NULL,488},
  {"OFF_QLIST_NPAGES",  "qfile_List_cache","n_pages",        NULL, 28},
  {"OFF_QPOOL_NENT",    "qfile_List_cache_entry_pool","n_entries",NULL,8},
  {"OFF_LK_NUMTRANS",   "lk_Gl","num_trans",                 NULL,  0},
  {"OFF_LF_ALLOCCNT",   "sessions","alloc_cnt",              NULL, 12},
  {"OFF_LF_ALLOCCNT2",  "catalog_Hashmap","alloc_cnt",       NULL, 12},
  {"SZ_SESSION_STATE",  NULL,NULL,"session_state",  312},
  {"SZ_CATALOG_ENTRY",  NULL,NULL,"catalog_entry",   48},
  {"SZ_FPCACHE_ENT",    NULL,NULL,"fpcache_ent",    120},
  {"SZ_LK_TRAN_LOCK",   NULL,NULL,"lk_tran_lock",   152},
  {"SZ_CSS_CONN_ENTRY", NULL,NULL,"css_conn_entry", 480},
  {"SZ_VACUUM_DATA",    NULL,NULL,"vacuum_data",    184},
  /* ---- Buffer pool BCB direct read (cub_top's pgbuf section, volmap --bufmap) ----
     The BCB array lies beyond the pgbuf_Pool.BCB_table pointer, so it is extracted in
     the type+member form. */
  {"OFF_PGBUF_BCBTAB",  "pgbuf_Pool","BCB_table",            NULL,  8},
  {"SZ_PGBUF_BCB",      NULL,NULL,"pgbuf_bcb",               144},
  {"OFF_BCB_VPID",      NULL,"vpid","pgbuf_bcb",             44},
  {"OFF_BCB_FLAGS",     NULL,"flags","pgbuf_bcb",            64},
  {"OFF_BCB_OLDEST_LSA",NULL,"oldest_unflush_lsa","pgbuf_bcb",128},
  {"OFF_BCB_IOPAGE",    NULL,"iopage_buffer","pgbuf_bcb",    136},
  {"OFF_IOBUF_IOPAGE",  NULL,"iopage","pgbuf_iopage_buffer",   8},
  {"OFF_LOG_NXIO_LSA",  "log_Gl","nxio_lsa",                 NULL,  48},
  {"OFF_LOG_APPEND_LSA","log_Gl","append_lsa",               NULL,  280},
  {"OFF_LOG_EOF_LSA",   "log_Gl","eof_lsa",                  NULL,  488},
  /* ---- Engine statistics direct read (capacity axis: hit ratio, engine IOPS demand) ----
     pstat_Global.global_stats is a pointer to a UINT64 array, and which index holds
     which counter is fixed at runtime by pstat_Metadata[psid].start_offset, so only
     the two structs' offsets are extracted. */
  {"OFF_PSTAT_NVALS",   "pstat_Global","n_stat_values",       NULL,    0},
  {"OFF_PSTAT_GSTATS",  "pstat_Global","global_stats",        NULL,    8},
  {"OFF_PSTAT_INIT",    "pstat_Global","initialized",         NULL,   44},
  {"SZ_PSTAT_METADATA", NULL,NULL,"pstat_metadata",                   56},
  {"OFF_PSTATMD_PSID",  NULL,"psid","pstat_metadata",                  0},
  {"OFF_PSTATMD_START", NULL,"start_offset","pstat_metadata",         20},
  /* Engine-maintained buffer pool aggregates (zone counts, victim candidates, dirty count):
     kept by the engine for its own LRU and flush control, so read without a BCB scan */
  {"OFF_PGBUF_LRULIST", "pgbuf_Pool","buf_LRU_list",          NULL, 56},
  {"OFF_PGBUF_NLRU",    "pgbuf_Pool","num_LRU_list",          NULL, 40},
  {"OFF_PGBUF_QUOTA",   "pgbuf_Pool","quota",                 NULL, 328},
  {"OFF_QUOTA_NPRIV",   NULL,"num_private_LRU_list","pgbuf_page_quota", 0},
  {"OFF_PGBUF_MONITOR", "pgbuf_Pool","monitor",               NULL, 280},
  {"OFF_MON_DIRTIES",   NULL,"dirties_cnt","pgbuf_page_monitor",      0},
  {"SZ_PGBUF_LRU",      NULL,NULL,"pgbuf_lru_list",                 128},
  {"OFF_LRU_C1",        NULL,"count_lru1","pgbuf_lru_list",         80},
  {"OFF_LRU_C2",        NULL,"count_lru2","pgbuf_lru_list",         84},
  {"OFF_LRU_C3",        NULL,"count_lru3","pgbuf_lru_list",         88},
  {"OFF_LRU_VICT",      NULL,"count_vict_cand","pgbuf_lru_list",    92},
  /* Hash map kind and object lock pools: lockfree_hashmap::m_type is OLD(0)/NEW(1); only OLD
     keeps the lf freelist whose alloc_cnt is read */
  {"OFF_SESS_HTYPE",    "sessions","m_type",                  NULL, 400},
  {"OFF_CAT_HTYPE",     "catalog_Hashmap","m_type",           NULL, 400},
  {"OFF_LK_HTYPE",      "lk_Gl","m_type",                     NULL, 472},
  {"OFF_LK_RES_ALLOC",  "lk_Gl","alloc_cnt",                  NULL, 84},
  {"OFF_LK_ENTFL",      "lk_Gl","obj_free_entry_list",        NULL, 480},
  {"OFF_LFFL_ALLOC",    NULL,"alloc_cnt","lf_freelist",             12},
  {"SZ_LK_ENTRY",       NULL,NULL,"lk_entry",                       128},
  {"SZ_LK_RES",         NULL,NULL,"lk_res",                         120},
};
#define NTG ((int)(sizeof TG/sizeof TG[0]))
static long GOT[NTG]; static int OKF[NTG]; static char NOTE[NTG][160];

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: dwoff <libcubrid.so> [--emit-c|--emit-py|--emit-tbl]\n"); return 1; }
    int emit_c=0,emit_py=0,emit_tbl=0;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--emit-c")) emit_c=1;
        else if(!strcmp(argv[i],"--emit-py")) emit_py=1;
        else if(!strcmp(argv[i],"--emit-tbl")) emit_tbl=1;
    }
    double t0=now_ms();
    int fd=open(argv[1],O_RDONLY); if(fd<0){ perror("open"); return 1; }
    struct stat st;
    if(fstat(fd,&st)!=0 || st.st_size<(off_t)sizeof(Elf64_Ehdr)){ fprintf(stderr,"not ELF64\n"); return 2; }
    unsigned char*m=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(m==MAP_FAILED){ perror("mmap"); return 1; }
    size_t fsz=(size_t)st.st_size;
    #define DW_IN_FILE(off_,len_) ((uint64_t)(off_) <= (uint64_t)fsz && \
                                   (uint64_t)(len_) <= (uint64_t)fsz - (uint64_t)(off_))
    Elf64_Ehdr*eh=(Elf64_Ehdr*)m;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG)||eh->e_ident[EI_CLASS]!=ELFCLASS64){
        fprintf(stderr,"not ELF64\n"); return 2; }
    #undef DW_IN_FILE
    int fl=0, r=dw_sections(m,fsz,&fl);
    if(r==-1){ fprintf(stderr,"section header table outside the file\n"); return 2; }
    if(r==-2){ fprintf(stderr,"section name table outside the file\n"); return 2; }
    if(fl&DWF_COMPRESSED){ fprintf(stderr,"compressed debug sections (-gz, SHF_COMPRESSED) - not supported; decompress with objcopy --decompress-debug-sections\n"); return 3; }
    if(!DI||!AB){
        fprintf(stderr,"%s\n",(fl&DWF_ZDEBUG)?"old-style compressed debug sections (.zdebug) - not supported"
                             :(fl&DWF_DEBUGLINK)?"debug info is in a separate file (.gnu_debuglink) - run dwoff on that file"
                             :"no debug sections - stripped binary");
        return 3; }
    Elf64_Shdr*sh=(Elf64_Shdr*)(m+eh->e_shoff);
    build_index();
    { const char *vs[PRE_MAX], *ts[PRE_MAX]; int nv=0, nt=0;
      for(int i=0;i<NTG;i++){ if(TG[i].sym && nv<PRE_MAX) vs[nv++]=TG[i].sym; if(TG[i].type && nt<PRE_MAX) ts[nt++]=TG[i].type; }
      prescan(vs,nv,ts,nt); atexit(prescan_free); }
    if(!emit_c&&!emit_py&&!emit_tbl)
        printf("dwoff - DWARF offset extraction\n  target: %s\n  .debug_info %.1fMB, %d CUs, DWARF v%d\n\n",
               argv[1],DI_SZ/1048576.0,NCU,CU[0].ver);
    for(int i=0;i<NTG;i++){
        GOT[i]=-1; OKF[i]=0; NOTE[i][0]=0;
        if(TG[i].type && TG[i].member){
            long off=-1;
            if(member_of_type(TG[i].type,TG[i].member,&off)==0){ GOT[i]=off; OKF[i]=1;
                snprintf(NOTE[i],sizeof NOTE[i],"struct %s . %s",TG[i].type,TG[i].member); }
            else snprintf(NOTE[i],sizeof NOTE[i],"struct %s - member %s not found",TG[i].type,TG[i].member);
        } else if(TG[i].type){
            long sz; const char*rn=NULL;
            if(sizeof_type(TG[i].type,&sz,&rn)==0){ GOT[i]=sz; OKF[i]=1;
                snprintf(NOTE[i],sizeof NOTE[i],"struct %s",TG[i].type); }
            else snprintf(NOTE[i],sizeof NOTE[i],"type not found");
        } else {
            die_t d; unsigned long toff=0;
            if(resolve_symbol(TG[i].sym,&d,&toff)==0){
                long off=-1; char path[320]={0};
                if(find_member(&d,TG[i].member,0,0,&off,path,sizeof path)==0){
                    GOT[i]=off; OKF[i]=1;
                    snprintf(NOTE[i],sizeof NOTE[i],"%s(%ld) . %s",
                             d.name?d.name:"(anonymous)",d.bsize,path);
                } else snprintf(NOTE[i],sizeof NOTE[i],"%s(%ld) - member %s not found",
                                d.name?d.name:"(anonymous)",d.bsize,TG[i].member);
            } else snprintf(NOTE[i],sizeof NOTE[i],"symbol resolution failed");
        }
    }
    if(emit_tbl){
        /* Portable offset file: extracted once where DWARF is available and shipped with the
           binary to a stripped production host.  cub_top --offsets <file> takes it as a
           candidate table, which must still pass that environment's three gates. */
        printf("# cub_top offsets table (dwoff --emit-tbl)\n");
        printf("#meta source=%s\n",argv[1]);
        { struct stat st2; if(stat(argv[1],&st2)==0)
              printf("#meta size=%lld mtime=%lld\n",
                     (long long)st2.st_size,(long long)st2.st_mtime); }
        for(int i=0;i<NTG;i++) if(OKF[i]) printf("%s %ld\n",TG[i].key,GOT[i]);
        /* st_size of the root symbols: cub_top matches a table to a build by these */
        { static const char *mangled[]={"_ZL10pgbuf_Pool","xcache_Global","_ZL16qfile_List_cache",
              "_ZL27qfile_List_cache_entry_pool","_ZL21fpcache_Entry_counter","lk_Gl","_ZL8sessions",
              "_ZL15catalog_Hashmap","_ZL16css_Num_max_conn","_ZL19css_Num_active_conn",
              "_ZL11vacuum_Data","log_Gl","pstat_Global","pstat_Metadata"};
          static const char *plain[]={"pgbuf_Pool","xcache_Global","qfile_List_cache",
              "qfile_List_cache_entry_pool","fpcache_Entry_counter","lk_Gl","sessions",
              "catalog_Hashmap","css_Num_max_conn","css_Num_active_conn",
              "vacuum_Data","log_Gl","pstat_Global","pstat_Metadata"};
          enum { NR=(int)(sizeof plain/sizeof plain[0]) };
          unsigned long sz[NR]; memset(sz,0,sizeof sz);
          for(int i=0;i<eh->e_shnum;i++){
              if(sh[i].sh_type!=SHT_SYMTAB && sh[i].sh_type!=SHT_DYNSYM) continue;
              if(sh[i].sh_entsize!=sizeof(Elf64_Sym) || sh[i].sh_link>=eh->e_shnum) continue;
              Elf64_Shdr *st_=&sh[sh[i].sh_link];
              if(sh[i].sh_offset>fsz || sh[i].sh_size>fsz-sh[i].sh_offset) continue;
              if(st_->sh_offset>fsz || st_->sh_size>fsz-st_->sh_offset) continue;
              const Elf64_Sym *sy=(const Elf64_Sym*)(m+sh[i].sh_offset);
              const char *str=(const char*)(m+st_->sh_offset);
              long cnt=(long)(sh[i].sh_size/sizeof(Elf64_Sym));
              for(long k=0;k<cnt;k++){
                  if(sy[k].st_name>=st_->sh_size || sy[k].st_shndx==SHN_UNDEF) continue;
                  if(ELF64_ST_TYPE(sy[k].st_info)!=STT_OBJECT) continue;
                  const char *nm=str+sy[k].st_name;
                  if(!memchr(nm,0,st_->sh_size-sy[k].st_name)) continue;
                  /* as cub_top's lookup: exact, or "name." (-flto .lto_priv.N) */
                  for(int j=0;j<NR;j++){
                      size_t a=strlen(mangled[j]), b=strlen(plain[j]);
                      if(!sz[j] && ((!strncmp(nm,mangled[j],a) && (!nm[a]||nm[a]=='.')) ||
                                    (!strncmp(nm,plain[j],b) && (!nm[b]||nm[b]=='.')))){ sz[j]=sy[k].st_size; break; } }
              }
          }
          for(int j=0;j<NR;j++) if(sz[j]) printf("SYMSZ_%s %lu\n",plain[j],sz[j]); }
        return 0;
    }
    if(emit_c){
        printf("/* Generated by dwoff - do not edit by hand */\n");
        for(int i=0;i<NTG;i++) if(OKF[i]) printf("#define %-22s %ld\n",TG[i].key,GOT[i]);
        return 0;
    }
    if(emit_py){
        printf("# Generated by dwoff - do not edit by hand\nLAYOUT={\n");
        for(int i=0;i<NTG;i++) if(OKF[i]) printf("  \"%s\": %ld,\n",TG[i].key,GOT[i]);
        printf("}\n");
        return 0;
    }
    printf("%-22s %8s %8s %-7s %s\n","key","extracted","existing","verdict","resolution path");
    int pass=0,fail=0,diff=0;
    for(int i=0;i<NTG;i++){
        const char*v;
        if(!OKF[i]){ v="fail"; fail++; }
        else if(GOT[i]==TG[i].expect){ v="match"; pass++; }
        else { v="differ"; diff++; }
        if(OKF[i]) printf("%-22s %8ld %8ld %-7s %s\n",TG[i].key,GOT[i],TG[i].expect,v,NOTE[i]);
        else       printf("%-22s %8s %8ld %-7s %s\n",TG[i].key,"-",TG[i].expect,v,NOTE[i]);
    }
    printf("\nmatch %d, differ %d, fail %d  |  %ld discarded on desync  |  %.0fms\n",
           pass,diff,fail,G_desync,now_ms()-t0);
    return (diff||fail)?1:0;
}
