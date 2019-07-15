L0:
         mov (4|M0)               r1.0<1>:ub    r1.0<0;1,0>:ub
         mov (8|M0)               r4.0<1>:ud    r0.0<8;8,1>:ud
         mov (8|M0)               r4.0<1>:ud    r2.0<2;2,1>:ud
         mov (1|M0)               r4.2<1>:ud    0xF000F:ud
         mov (16|M0)              r5.0<1>:ud    r1.0<0;1,0>:ud                   {@4}
         mov (16|M0)              r7.0<1>:ud    r1.0<0;1,0>:ud                   {@5}
         mov (16|M0)              r9.0<1>:ud    r1.0<0;1,0>:ud                   {@6}
         mov (16|M0)              r11.0<1>:ud   r1.0<0;1,0>:ud                   {@7}
         send.dc1 (16|M0)         null     r4      null    0x10000000  0x120A8000 {@1, $0} //    wr:9h+0, rd:0, Media Block Write msc:0, to #0
         mov (8|M0)               r112.0<1>:ud  r0.0<8;8,1>:ud
         send.ts (16|M0)          null     r112    null    0x10000000  0x2000010  {EOT, @1} //    wr:1+0, rd:0, fc: 0x10
L176:
