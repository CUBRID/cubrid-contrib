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
/* Compiled together with cub_top.c (check-cpucell.sh): cpu_cell() and sat_cell() are the
   dashboard's own code.  No cell may exceed its display width on worst-case input. */
int main(void){
    int bad=0;
    /* Exact cells: the label goes first, then the % sign, only as far as the width needs */
    static const struct { double v; const char *us, *sy; } X[]={
        {0,"us 0%","sy 0%"},{5,"us 5%","sy 5%"},{17,"us 17%","sy 17%"},{99,"us 99%","sy 99%"},
        {100,"us 100%","100%"},{100.4,"us 100%","100%"} };
    for(unsigned i=0;i<sizeof X/sizeof*X;i++){
        char c4[16],c5[16]; double vals_i=X[i].v;
        cpu_cell(c4,sizeof c4,"us",vals_i,7);
        cpu_cell(c5,sizeof c5,"sy",vals_i,6);
        int ok=disp_w(c4)<=7&&disp_w(c5)<=6&&!strcmp(c4,X[i].us)&&!strcmp(c5,X[i].sy);
        printf("  %6.1f%% -> us \"%s\"(%d) sy \"%s\"(%d) %s\n",vals_i,c4,disp_w(c4),c5,disp_w(c5),ok?"OK":"<<< wrong cell");
        if(!ok) bad++;
    }
    /* Saturated cores, in both languages and both widths (7 wide, 6 narrow) */
    for(int asc=0;asc<=1;asc++) for(int cap=6;cap<=7;cap++) for(int n=0;n<=9999;n=n?n*10+9:9){
        char c3[24]; g_ascii=asc; sat_cell(c3,sizeof c3,n,cap);
        int ok=disp_w(c3)<=cap;
        printf("  %s cap=%d sat=%-4d -> \"%s\"(%d) %s\n",asc?"ascii":"ko   ",cap,n,c3,disp_w(c3),ok?"OK":"<<< overflow");
        if(!ok) bad++;
    }
    printf("%s\n",bad?"FAIL":"PASS");
    return bad?1:0;
}
