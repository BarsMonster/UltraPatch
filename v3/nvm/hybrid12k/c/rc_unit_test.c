/* Bit-exactness unit test for the C range decoder + models (rc_models.h) against the
 * Python golden model. Run: python3 m4dev/testbench/rc_gen_vector.py  (writes /tmp/rcvec.bin)
 * then: cc -O2 -I c -o /tmp/rc_unit_test c/rc_unit_test.c && /tmp/rc_unit_test */
#include <stdio.h>
#include "rc_models.h"
static uint8_t buf[1<<16];
int main(void){
    FILE*f=fopen("/tmp/rcvec.bin","rb");
    if(!f){ fprintf(stderr,"run m4dev/testbench/rc_gen_vector.py first\n"); return 2; }
    size_t blen=fread(buf,1,sizeof buf,f); fclose(f);
    int vg[]={0,1,2,3,7,0,5,100,2,2,0,1,255,1024,3,0,0,7}, ng=18;
    int vr[]={0,1,8,3,15,2,40,7,0,256,1,1,9}, nr=13;
    int vf[]={0,1,1,0,1,1,1,0,1,0,1,1,0,0,1}, nf=15;
    RDec r; rd_init(&r,buf,blen);
    UGolomb gg; ug_init(&gg,'g',0); UGolomb gr; ug_init(&gr,'r',3); Flag1 fl; fl_init(&fl);
    int ok=1;
    for(int i=0;i<ng;i++){ uint32_t v=ug_decode(&r,&gg); if((int)v!=vg[i]){ printf("G[%d] got %u want %d\n",i,v,vg[i]); ok=0; } }
    for(int i=0;i<nr;i++){ uint32_t v=ug_decode(&r,&gr); if((int)v!=vr[i]){ printf("R[%d] got %u want %d\n",i,v,vr[i]); ok=0; } }
    for(int i=0;i<nf;i++){ int b=fl_decode(&r,&fl); if(b!=vf[i]){ printf("F[%d] got %d want %d\n",i,b,vf[i]); ok=0; } }

    /* FreqModel (literal real prior) + ByteVarint */
    FILE*mf=fopen("/tmp/rcvec2.meta","r");
    if(mf){
        static int lt[64],lb[64]; static long dv[64]; int nl=0,nd=0; char tok[64];
        while(fscanf(mf,"%63s",tok)==1){ int t,b; if(sscanf(tok,"%d:%d",&t,&b)==2){ lt[nl]=t; lb[nl]=b; nl++; } else { dv[nd++]=atol(tok); } }
        fclose(mf);
        FILE*bf=fopen("/tmp/rcvec2.bin","rb"); size_t bl=fread(buf,1,sizeof buf,bf); fclose(bf);
        RDec r2; rd_init(&r2,buf,bl);
        BitTree M0,M1; bt_init(&M0); bt_init(&M1); ByteVarint bv; bv_init(&bv);
        for(int i=0;i<nl;i++){ int s=bt_decode(&r2,lt[i]?&M1:&M0); if(s!=lb[i]){ printf("LIT[%d] got %d want %d\n",i,s,lb[i]); ok=0; } }
        for(int i=0;i<nd;i++){ long v=bv_decode(&r2,&bv); if(v!=dv[i]){ printf("DV[%d] got %ld want %ld\n",i,v,dv[i]); ok=0; } }
        printf("  (BitTree literals=%d + ByteVarint=%d checked)\n",nl,nd);
    } else printf("  (skipped FreqModel/ByteVarint: no rcvec2.meta)\n");

    printf(ok? "C unit test: ALL OK (range decoder + UGolomb g/r + Flag1 + BitTree + ByteVarint bit-exact)\n":"C unit test: FAIL\n");
    return ok?0:1;
}
