L0:
         mov (4|M0)               r1.0<1>:ub    r1.0<0;1,0>:ub
         shl (1|M0)               r2.0<1>:ud    r0.1<0;1,0>:ud    0x4:ud
         mov (1|M0)               r2.1<1>:ud    r0.6<0;1,0>:ud
         mov (8|M0)               r4.0<1>:ud    r0.0<8;8,1>:ud
         mov (2|M0)               r4.0<1>:ud    r2.0<2;2,1>:ud                   {I@2}
         mov (1|M0)               r4.2<1>:ud    0xF:ud
         mov (16|M0)              r5.0<1>:ud    r1.0<0;1,0>:ud                   {I@6}
(W)      sync.nop                             null                             {I@1}
         send.dc1 (16|M0)         null     r4      null:0    0x0         0x40A8000  {$0} //    wr:2h+0, rd:0, Media Block Write msc:0, to #0
         send.gtwy (8|M0)         null     r80     null:0    0x0         0x02000000 {EOT}
L176:
