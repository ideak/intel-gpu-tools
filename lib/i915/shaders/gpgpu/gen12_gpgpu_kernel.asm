L0:
         mov (4|M0)               r1.0<1>:ub    r1.0<0;1,0>:ub
         shl (1|M0)               r2.0<1>:ud    r0.1<0;1,0>:ud    0x4:ud
         mov (1|M0)               r2.1<1>:ud    r0.6<0;1,0>:ud
         mov (8|M0)               r4.0<1>:ud    r0.0<8;8,1>:ud
         mov (2|M0)               r4.0<1>:ud    r2.0<2;2,1>:ud                   {@2}
         mov (1|M0)               r4.2<1>:ud    0xF:ud
         mov (16|M0)              r5.0<1>:ud    r1.0<0;1,0>:ud                   {@6}
         send.dc1 (16|M0)         null     r4      null    0x0         0x40A8000  {@1, $0} //    wr:2h+0, rd:0, Media Block Write msc:0, to #0
         mov (8|M0)               r112.0<1>:ud  r0.0<8;8,1>:ud
         send.ts (16|M0)          null     r112    null    0x10000000  0x2000010  {EOT, @1} //    wr:1+0, rd:0, fc: 0x10
L160:
