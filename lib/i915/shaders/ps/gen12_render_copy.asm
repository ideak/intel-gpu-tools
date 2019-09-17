L0:
(W)      mad (8|M0)               acc0.0<1>:nf  r6.3<0;0>:f       r2.0<8;1>:f       r6.0<0>:f
(W)      mad (8|M0)               r113.0<1>:f   acc0.0<8;1>:nf    r3.0<8;1>:f       r6.1<0>:f
(W)      mad (8|M0)               acc0.0<1>:nf  r6.3<0;0>:f       r4.0<8;1>:f       r6.0<0>:f
(W)      mad (8|M0)               r114.0<1>:f   acc0.0<8;1>:nf    r5.0<8;1>:f       r6.1<0>:f
(W)      mad (8|M0)               acc0.0<1>:nf  r6.7<0;0>:f       r2.0<8;1>:f       r6.4<0>:f
(W)      mad (8|M0)               r115.0<1>:f   acc0.0<8;1>:nf    r3.0<8;1>:f       r6.5<0>:f
(W)      mad (8|M0)               acc0.0<1>:nf  r6.7<0;0>:f       r4.0<8;1>:f       r6.4<0>:f
(W)      mad (8|M0)               r116.0<1>:f   acc0.0<8;1>:nf    r5.0<8;1>:f       r6.5<0>:f
(W)      send.smpl (16|M0)        r12      r113    null    0x0         0x8840001  {@1, $0} //    wr:4+0, rd:8, fc: 0x40001
         mov (16|M0)              r113.0<1>:f   r12.0<8;8,1>:f                   {$0.dst}
         mov (16|M0)              r115.0<1>:f   r14.0<8;8,1>:f
         mov (16|M0)              r117.0<1>:f   r16.0<8;8,1>:f
         mov (16|M0)              r119.0<1>:f   r18.0<8;8,1>:f
(W)      send.rc (16|M0)          null     r113    null    0x0         0x10031000 {EOT, @1} //    wr:8+0, rd:0, Render Target Write msc:16, to #0
L224:
