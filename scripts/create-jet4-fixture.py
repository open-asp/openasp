#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Materialize the minimal Jet 4 database used by native MDB regression tests."""

from __future__ import annotations

import base64
import hashlib
from pathlib import Path
import sys
import zlib


EXPECTED_SIZE = 98_304
EXPECTED_SHA256 = "521c6f8a60f94273866367f69c772eb590fc7fcb8bc91bcb134d567b1e9b6566"
COMPRESSED = (
    "c-rlq33Oc5d4T`>=4tj#vSe&y<3VPNgOt%o7Baz(Ml+IaAz8L18yu@x%Z%i)C5@sP3kzGeVnPpaPK$d|P6>swG%+P@N(iBC0yIFN"
    "JtZM_=|)bHvXpX4V+VSAa%f5M^uPDrH}huEYGcXf|DNu=`|kGN?JfU(_gw)4Xis(zMY=~KzUFAs7j6J_f~SW}JzcgJx4-vz|4988"
    ";DN&r9ZTJDf7Q!l-#_ht_KhDu*!$CWe)v$+Z|;6}^H}`Ub&sBW;G?y#T=VX!^1HkK6b{zf-+klBJG&Nz{GWfa`bWPW+ckLkq18{E"
    "JoGi|%KJY4^L5+bd+?E?eY@}dpObf8aYz5(th_q>+OJMTPp2$@+w{nR#m>|3RrLI0hZ&%z(hmed5ClOG1VIqQxq&gD?Nhul7V$9I"
    "7ZiBLGwPvpf*=TjAP9mW2!bFMAgQxKVgKsji7ZnAcA5c7^l_maA??NBfMQU@Fad%=nOXBHF$B}?PM($xOlY;K#vsDOD$>aIuyPMo"
    "$mJf3grgmU3Xh^3j}r7yxrZg4l;*Jv)39?7J6f`3oM9q{MkWTA>fjzOOe{B7!o!P+74-065es^_F?+clB3DXUcqUgi?qN<BFp!;H"
    "mXlpo&#sfVL5>WOlmlxkSB8mum~twSH-j5O5Qib$qR<OTENd9Lao-I+XcFjJDqW%&as+=hd&YG;3`0cP?a%}RXz$0Ei_snzF=BW;"
    "1e?))5RX*YgyB_afl-vse;eTd#vVa`uQ+bScoEo-DMhiA++_>;#!-$nor)dW;8v0A5sbA3UAd$oOoykw7`9<3ZXbd^jM*Z}*bhr`"
    "+}mM~$YT;y?tn3r5QW<<ywaHhER}+Se_qk=G`=i;Zx@G?M!>&f(d~_K^u}fSIV@UNWx8M1`K|fM4!}#eB(KbR&DcQYCyaKQ@m3dI"
    "+se;J(U;x4<BHDf<UXRe+IhFp`V=plnG2;0D?tzhK@bE%Tx=-paW~($F-_~JnN0uZs=1n0SV}PHBB`Vl>uR*7;Ice*l@(Gxc{-Eo"
    "<F;(gS5t{CsK=Tih-zpnsy|IaiQ6L-z?f86xx(m0#jpqVN_CbiMrKs=Hlezjqommk>Y18Km7t|m3|aunNWz^kog$VmiIgUSAP9mW"
    "2!bF8V!`VFzjX1vOUYJt^Yni!s^!+S{$GYc7n^gED*U>dY47}tawh|z&7{5gR<mLhfug_K&-SqBul6%T30hLAGiR7xn?9iAc}zXV"
    "koP_wW0VN}KPko)dSJhN-OXQ84~rMB3EX1hg|apMx;ytpwmtniw#r}+6c{t$FS18)&&M(32!-M8^N{$xP5k!Z?@}?d0WfsL@EW<m"
    "2!&{e@;qtzB`bnUFoh-b%9kJrf*=TjAPC}O*Z+Uy<ogQKwEBM$>bqQr=VloOz0^E25tGgfkn`JFo@%ZZo~<s`|Ds$iKb2PhSz!-L"
    "&bI(*4|CoZ3ep2|#|6;Cjaqas>iq+#IS-=tZ%4g<1odb>`@oIAMn5kk;5IR}KHJ5pk6$5Nw&78q(Ej;U?p~qU8{<tL%9z&jbf^I_"
    "6<-^lNWdPn4bNbD`f~BzfW}er%j2k1##tdIj3ceko{3}TafT*~bD5ZoZj>|0H2Ab%`iqkSeg(!o#Nlk}dgirA<0Aw?5ClOG1VLQT"
    "Ft$_wlo?iv_5Z3or`(UZd)zDBue-kL+U&BqUUD9BUhn+1<J*pXj;kDR*&ndqWG}J5YP-`GvYoMh&ziJ;&ibF0Z&;`{34$O9;+*3<"
    "7m0Y!MIzRVt728^Tb~Td!s++tA`x~D(wI0mHCw-65?MG*z`|)Cg18_~VwK(NRHnMoGrWp>#r2cnp--"
    "G3UVbp4$+m)U=5AJM(gnwnd-zXV^I&sxYwOm1`}&%<Z{5dlWNvH5tEG9LbYz-idnDGSIoLFZu1H(_78%B_ImNc#(mc2`(zEZToqd"
    "{zqI>j3VuPBKLvz~K8r#_?ot)BXbRVW5TwIz9=0g`DvdxZkVIVErk(OOsTV(>pMo$@`DC1@fs%D(`#J9$F%FyPFbABl-+*3KC$-S"
    "j{G!h>a?rzPUM`~*kE+)-oU%acUIX1epr#&K^J(_cT-_}^$*7lt}Tib-QmpUUs5ClOG1VIq<ipmbVCe!~F)#~-tPXIGmsKzcZ{a@"
    "Rmwhv+e8x?vG1M{fHgZP#Mb#xH#Ku|LW{&fXc$U*#v2h@2%EOn1MEr{_QxA(||7G32QlTf`UwCHATDH0m=1YcfW*VJ`=UeU)FOk%"
    "9yn3gdUwGlxO1VIo4K@i0G(f?bVrdjC!GqX^=E-?MSENx-BMbRxLIW$mKq+86o#iCoRa@3$qA3i9v>lTM@amoROvSQs*qFWZ}mQp"
    "#^P*$c}7VDNvbjuPs3{kdJw=B~wm+F?~a$utD3OO)QcBLGcDEq8#xmLGS$f1g|&*+xba?GNvQn!3gxA^5KMp>0^3Fww;Ij&K5m2S"
    "COw_GDfI?7h)mdkX@O5Ng<gCJ$990VzI=@z$c@#q$>93LsWT(^|#mQ~cN34$O9f*=Tjn4A9ZbJI(Gf*=TjAP9mW2!goy_5UKO27("
    "|6f*=TjAP9mW{huHRf*=TjAP9mW2-5!vf*=TjAP9mW2!dG9Fm|_1U;n=Zp7umM4$pVpyWL**Gp;YXK6ZZB+3#HDeARK(@rM1deTD"
    "s58`T*>5ClOG#C%cAm!9QI%X%0*JF;^8zaDzK{yDkl&vdQO4r7pjAoSuEMOy;epa=G%y%&;rbi6tY`C9|{zyT>PzzQ&dNqHO-iOQ"
    "E>{=KifU9azX2DW1oQ5eA}QRssh4B<Y4i3S1cl}oftYDv~*ZMB+!dK6%pQb0TGhe6!BQOFo<LIHgcM*)wb05&rL=Cpv0r@lAHt7e"
    "Nwt0ErRST!vuW(<?xk5zNLy2kLo;9Iu^U>R_S61WjVM`0L4$1!F%48UR$#w>pA;!lANc+mp&(hGhYZo}>RtgTLFkrnf`g}Eq?2@2"
    "N%Z^LaUbO5te3HjT#nf6krNvansGKRU=8s883TV*CO)0ByA6V(vM1ZR|(GG$_|m{0_3en7Ovw284P6KfK=8Wag#3;EmXOhPbaLSb"
    "y*BpPEc*@DOYGMDv`zpc+CUX(sN*Z=)XB2j(M#kOX(EEg2Rd=H_^9??Vzc&uugvs0asfUQ`sQPJ*Ek!T&{Z?##iK7&Mq5Q0XLTmb"
    "Sne^$HCAi3<z(zJO*y!@nArc5-9i5{=+<o~B$UMsXyui)8JuNB>+^@c*pPSsF1K>l`pRwYc4u-?sj&?ICd)j<9h$T|V0NU%dBHXx"
    "Il@{*x28&3${N3+u`)*jc!1<(xK`x6C@olv;Szga;iML082{kZpP!SyJ`JI(q9)23#e@AIhKgQY$6;RY4&E_%^byts$qb_);3QYo"
    "11PeGZbGX7oSHzO2DAtGs2h;w5%Z4<YR2-d{`&^!r(AP9mW2!bF8;!_7>D?Ga01kZRzJyjm66@nlLf*=TjAP9ocSUCUU-hGz(|Ba"
    "w4{0w%T%Zw>v0t_tIj{fB5C2*g6vG)2O&BJ*7jfalk%KfIBrgma?3*>Lj`Wy!0Q_K6XO95tuXHOkc_+&diHOL|NVx9?~d1B_DwrD"
    "JQ>5w94Bngu;0P@%-{~xBw;GZk^&FHuyCrV|XPC3WzFEHV>@$@0(<i8&hQ)zf&C-;sk&+i{s>XYM&@4&e7aR0b6yk}ei?iL$YPWG"
    "W4jTggp-8!xu8^z-<VVDGl9YKGz|KBeUks(9o0yA9)@jSk5T$!(Hrhbh|%@ZHs_3qna^H=^Yr{K~$yk9_p>wD@<=2Bniy1x)8JZJ"
    "X&a<kHG0<RU8?EZmb?Rr6RYPVWp+_LWYeV9K1TG3bl^x4YdzHFC%+|4G#==E)p_g<&GmovlTHjnF(GQ866qQ@YDAc$G;N1ppV3{0"
    "P>{(tgB@tco4x$Lc6pY?csKa;Y7Ip?W_8Aexh?dpv2?o3=ze2hlF+w*oTl1~U0dB?X&cZa;&yW}x(aj*^IxFx{{jWC3J{t@SFSd7"
    "nzdRmTY!VXs)yM5)2FERc^`MeCg0~#QB7FRGUFs92_rje`fjeeT^Em-b&`bm9u<=5m}0kW=@vy*>T<xkhYItTd?h+)~!M!q7-Uo="
    "Z??5NDeZdn7nu?@DNzSNBUFa#YlmcI<|VYwrO)4{K8k65IpIVY*cmkbIs$uwsw|0*=)tdRU$VQ`l6OXpDj&qC%4KV921Nlj0`{-2"
    "S3+9vM}#_PXZYIR;YipxiC7lHQGUzs>3Kzz-)1kF;>MgW(}*9<GrT!v;P8Xua=d0n7ch2{!0S4!De<Ng{npFy))y5$<;>|uwWoM2"
    "M#_perzV>TErHE%S*Q3WFJs48}d<-X|PaB_@cb3IxQb=W_Y(shb`heg@LO0DJ+n^IC&U%s+I1*g({Ly=-%tc)a*k@$cbU9T)n-~}"
    "}Tcy;C9y1GiC53cM4)9TI*aM17f2mCeFr~DOQRn-vK6*nsZzrUt-d{v+l{Hy${Lf3cR1e-Q*?&wU6VM@`#>Og0E2ZY;N8UvLbEo#"
    "Fmu=;}glkw*VyOXi_5ZnTavRHXN7;XzUtXHw?T7wXVwtt#n4G{X;gyo(IriSid;i{rgmA|6S6oO#T@2?F98cTyUEL3F*Z5fF}|48"
    "?suPKIEPIQLigM)GVkZNu}+S=J2iCa4d6BA0Hsxk=!J*!${y%;_b?@RiEy-A3TMsN0oyOYrEYwOt?MVEvxP+8?`iFNgibdQWpYy{"
    ">hR$j#u=BVkFl$R-vvwMR7arYo^I`Kw?n|D-ZcwSfW-2Mp9pH9H)mfz^}PrqyMdea0I>F@T9wosm%ajSt^7{{}<2C#u}zZOjhu15"
    "*lm23n42H`MNp-TvlTXAcH&C;b6HlZsI*DhVOtG~Xlm9|E-RbspGcZjRdB?bxDFIIB!p)dagJBD{^pIk=3hcSn78xXF>ufMZi!ZN"
    ")cYA}2gZXwKlP?U5H9<9Z#0`ufY(||S}hS!r;4?z*q57p@E7v?%q4{K#T_zm^IpC$V7w;QkKJdZK?oi{IkOswyKJqAsmc<yS$eV="
    "fTVu<V`K90yg;|~crZCD!(D5p>=#y7J*nJZt`_@fVF$MASWF4bY=uL|~&>YSc2+ddM=s~bF*u`g`KJ6$K{laG)lF?WL~DS$5R=pG"
    "iOY!q=jWO+3gUPiv|M_ep6qPMY;%UE_P^paHNm7+@6>ayJQ7OurS1W;ZO%b}N|YQ`-b68RexdCT@`H<Y&%V@5Dcr&t0*vj<R0CHf"
    "Cw8?6%e<@2HcPu7=s{f=T@Z^mPN1pr^pc(za0%~U;$KUMJ8xvIkOzJbq+xc^-+zRd-w6E}~uzLOOw-zzEYsf;nJLSqv|Ds=$#MT0"
    "EN4k>u&3hg{FtHIvhXd=OvrYZW$a!BC=v3Tq;sXH2iAHMB{BK4-&P*v><ho!YTU^(IiliJ$d`_y;5pz?6F-&*CxXBm8i;P?J2`z2"
    "mXSUd|wzuLv)iRY3823L5eydgbcrkxT_g+I)jg-YjEOqo|-lXhNvfe8%-*%V$^elUPJye{mos%WeU1uAO7flx&-RNGil*Axf_YSz"
    "@$t!bzkFL&8DcMnF_m5buz(Z=Y&a40^Mj2=kxRQ%|)BRZ16r!9Om8pg-2o`Gm}U|o6j+Uh_cfDd&TAk?~HU3p_;W2maJI?xyhHHJ"
    "}|(O{VHmWFlZ;ZPX81OBEOe^b*M&7j|g(7N(aFjQ9^TvHoZyXHD>!nNq(vhUg$Pb7Vf2a?gDNOZ)vX{ay$TQ2)m8~qwp>kd}eRR_"
    "YgHU5fF(^`K;RaK~|qOP%~y29@d`a^50s{H;n!ST=UXlVR=I+ovM|K#83va`Kf$E2dB>;?X$H{1BC*SPG?`6ZZsI?<l=^n1!Dspn"
    "rS$1pjW1*hhQ6PTDLWS>IMpK6>Xgt8-I`Jj<G<-uzh=bDTB?A5#GCf_FY|GCJ|UW)Aemp^-5wYkZ6%5jgm$k(13hiBdIbxPzo+id"
    "!Og&d3F<S$?~+i@l)#(fsS<P&SBD!>0MuOF&x{}MUoG+X%%*z@COan-ZWq5h5U#Z1@&F|iKgoZEla*S|_b`-kw{Pe|=)gXoL<#ky"
    "!Szy50VUpmMBKlM6&)8jLYbI(QoxnQ>SPx6VmYvyVFUu8w^$v*g3R4dih1<nyoA1xLZ#xM2UqG<P8f*=TjAP9mW2;x%%W3^^|rT!"
    "W??HTp-dbW7hc~*O#azE_;9rtZ+llueLFI=y<{?b+Ns&FlH{ju|ybKJSt`QMH=9j`jR?+7{kjuj5O<3sy|ebD|z`v&_-+jF*W+Wx"
    "@iv$<>^S#P%vTmRScuH_#rot90OTFVd2e`cn7CkTQdW{xT*kn@*Bk5xOt4h$^1lNrALVN?HGU+VwWzdY%==gj+~sc-&U-`1k%UVE"
    "_h#<kmi`kVVLea>C4y*~bnr!I>gTMEB_*NZn?x%b4+YVN=3i7hXe-hZF}-D}ru|APD86Mu2)>EZ8u@c17t|JhHvk_QhiI<ex3AO7"
    "=_-4E<~=RXc#clxPeqgXy!{dguPYJTR_`N^C%dBbeXoRXh8?$mMqGpFQd&NM$cG3Dn(djsz=R+H{@o-?P5HDt~zW|uKz<}CS6h}V"
    "7oD^n8-g>qsj0{_2NsWX}S^<<pRXMPQ0k7wuP3|9POwP`2*ulYoO$agYC)Sb?A5z?8<<V4NSubQ7<b$;@z=I7UxpI=jceoga}UsJ"
    "*SdM0r?|C#f!yvt~A|5ftqSIMtmWq#^c$**6QpI?@rUp7DaW%>El{;^+HD?ok`AioGOKShAtA~05IZP^iQ5n}<co?iG91VIo4F%7"
    "T$wVM7vzlFXa;_e|BfvA|E8wH=7CO#nSG4UB=SWME5LBBW}75}FC){Cl^Ow>`RHmEkJHmEkJHqLQv@a1mAvS|#U&+t)-{?Ym^kXe"
    "rB@8q7lSL=56Ew`@!o8apn#q)xjYL_4gf*=TjAP9n3aI|g<h_zfNz$e#%$*j~E%~^Z7OvKP;pWB2U%z*I({2~E6bYRq^7@c;?1p)"
    "VnWo6uQ>Ahi0b_mAg(tuh*Ua@YE6~*ft*cmJ_9A|F<^jBc@r2hFE&tav=;j*kHE_UF4M2wyD1p)2QfH~$%0!GB5wtD_bME>kp#yU"
    ")84ep%)Dqp_y=Su<pAC3xt=>"
)


def main() -> int:
    """Decode the source-controlled fixture only after size and hash validation."""
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <output.mdb>", file=sys.stderr)
        return 2
    data = zlib.decompress(base64.b85decode(COMPRESSED))
    if len(data) != EXPECTED_SIZE or hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        print("embedded Jet 4 fixture failed integrity validation", file=sys.stderr)
        return 1
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
