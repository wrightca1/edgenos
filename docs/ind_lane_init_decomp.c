// _phy_wc40_independent_lane_init at 0x01599d10

int FUN_01599d10(int param_1,int param_2)

{
  int iVar1;
  ushort uVar3;
  uint uVar2;
  int iVar4;
  uint uVar5;
  undefined4 uVar6;
  char cVar7;
  int iVar8;
  int iVar9;
  ushort local_80 [2];
  undefined1 auStack_7c [4];
  undefined1 auStack_78 [32];
  undefined1 auStack_58 [64];
  
  iVar1 = DAT_01599d0c;
  iVar9 = *(int *)(*(int *)(*(int *)(FUN_01591d20 + DAT_01599d0c) + param_1 * 4) + param_2 * 4);
  iVar8 = *(int *)(iVar9 + 0x1a0);
  *(undefined4 *)(iVar9 + 0xb4) = 1;
  *(undefined4 *)(iVar9 + 0xf0) = 1;
  *(undefined4 *)(iVar9 + 0xf4) = 3;
  *(int *)(iVar9 + 0xac) = iVar8;
  *(int *)(iVar9 + 0xb0) = iVar8;
  *(undefined4 *)(iVar9 + 0xec) = 1000;
  *(undefined4 *)(iVar9 + 0xf8) = 2;
  if (((*(char *)(iVar9 + 0x17) == '\0') && ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0)) &&
     ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0x800)) {
    FUN_0137fb14(25000);
    iVar4 = *(int *)(iVar9 + 0x1c4);
  }
  else {
    iVar4 = FUN_0158f010(param_1,iVar9,0,0xffe0,0x8000);
    if (iVar4 < 0) {
      return iVar4;
    }
    iVar4 = FUN_0158ee1c(iVar9,0xffe0,0x8000,0,10000,0);
    if ((iVar4 == -9) && (iVar4 = FUN_01393e80(0xa00fe03), iVar4 != 0)) {
      FUN_013949c0(*(undefined4 *)(iVar1 + 0x1591e48),0xa00fe03,*(undefined4 *)(iVar1 + 0x1591d2c),
                   0x817,*(int *)(iVar1 + 0x1591d48) + 0x354,param_1,param_1,param_2);
    }
    FUN_0137fb14(25000);
    iVar4 = *(int *)(iVar9 + 0x1c4);
  }
  if ((iVar4 - 4U < 2) &&
     (iVar4 = FUN_0158fbdc(param_1,iVar9,
                           *(undefined4 *)
                            (*(int *)(iVar1 + 0x1591d48) + (uint)*(byte *)(iVar9 + 0x17) * 4 + 200),
                           0x820e,0x301,0xff0f), iVar4 < 0)) {
    return iVar4;
  }
  iVar4 = FUN_01594b7c(param_1,param_2,*(undefined1 *)(iVar9 + 0x17));
  if (iVar4 < 0) {
    return iVar4;
  }
  if (((*(char *)(iVar9 + 0x18) == '\x02') || (*(char *)(iVar9 + 0x18) == '\x05')) &&
     (iVar4 = FUN_01594b7c(param_1,param_2,*(byte *)(iVar9 + 0x17) + 1), iVar4 < 0)) {
    return iVar4;
  }
  iVar4 = FUN_0158f010(param_1,iVar9,0,0x82e6,0x3f0);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_0158f010(param_1,iVar9,0,0x82e8,0x3f0);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x82e3,0x3f00,0x3f00);
  if (iVar4 < 0) {
    return iVar4;
  }
  uVar3 = *(ushort *)(iVar9 + 0x2a4) & 0xf800;
  if (((uVar3 == 0x4000) || (uVar3 == 0x4800)) &&
     ((*(char *)(iVar9 + 0x18) != '\x02' &&
      ((*(char *)(iVar9 + 0x18) != '\x05' && (*(int *)(iVar9 + 0x1bc) != 0)))))) {
    iVar4 = FUN_0158fbdc(param_1,iVar9,1,(uint)*(byte *)(iVar9 + 0x17) * 0x10 + 0x80ba,0x800,0xc00);
    if (iVar4 < 0) {
      return iVar4;
    }
    uVar3 = *(ushort *)(iVar9 + 0x2a4) & 0xf800;
  }
  if (((((uVar3 == 0) || (uVar3 == 0x800)) || (uVar3 == 0x4000)) || (uVar3 == 0x4800)) &&
     ((*(char *)(iVar9 + 0x18) != '\x02' && (*(char *)(iVar9 + 0x18) != '\x05')))) {
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8000000,0x40,0x207c);
    if (iVar4 < 0) {
      return iVar4;
    }
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8104,0xff00,0xff00);
    if (iVar4 < 0) {
      return iVar4;
    }
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8357,0x400,0x600);
  if (iVar4 < 0) {
    return iVar4;
  }
  if (*(int *)(iVar9 + 0x294) == 2) {
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0xffc8,0,1 << (*(byte *)(iVar9 + 0x17) & 0x3f) & 0xffff);
    if (iVar4 < 0) {
      return iVar4;
    }
    *(undefined4 *)(iVar9 + 0x294) = 3;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8345,0xc000,0xc000);
  if (iVar4 < 0) {
    return iVar4;
  }
  uVar5 = 0;
  if (*(int *)(iVar9 + 0x204) != 0) {
    iVar4 = FUN_0158fbdc(param_1,iVar9,1,(uint)*(byte *)(iVar9 + 0x17) * 0x10 + 0x80b4,0x60,0xfc);
    if (iVar4 < 0) {
      return iVar4;
    }
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8390,0,0x40);
    if (iVar4 < 0) {
      return iVar4;
    }
    uVar5 = 4;
  }
  uVar2 = 0;
  if (*(int *)(iVar9 + 0x208) != 0) {
    uVar2 = 8;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8300,uVar2 | uVar5,0xc);
  if (iVar4 < 0) {
    return iVar4;
  }
  cVar7 = *(char *)(iVar9 + 0x18);
  if ((cVar7 == '\x02') || (cVar7 == '\x05')) {
    if (((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0) &&
       ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0x800)) {
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8368,0xc0,0xc0);
      if (iVar4 < 0) {
        return iVar4;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,
                           *(undefined4 *)
                            (*(int *)(iVar1 + 0x1591d48) + (*(byte *)(iVar9 + 0x17) + 1) * 4 + 200),
                           0x8368,0xc0,0xc0);
      if (iVar4 < 0) {
        return iVar4;
      }
      uVar3 = *(ushort *)(iVar9 + 0x2a4) & 0xf800;
      if (((uVar3 == 0x4000) || (uVar3 == 0x4800)) &&
         (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x18000007,4,7), iVar4 < 0)) {
        return iVar4;
      }
      cVar7 = *(char *)(iVar9 + 0x18);
    }
    if ((cVar7 != '\x02') && (cVar7 != '\x05')) goto LAB_01599fa8;
  }
  else {
LAB_01599fa8:
    if ((((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0) ||
        ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0x800)) &&
       (iVar4 = FUN_0158fbdc(param_1,iVar9,1,(uint)*(byte *)(iVar9 + 0x17) * 0x10 + 0x80ba,0x20,0x30
                            ), iVar4 < 0)) {
      return iVar4;
    }
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8309,0,0x20);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_01597f00(param_1,iVar9,1);
  if (iVar4 < 0) {
    return iVar4;
  }
  if ((*(char *)(iVar9 + 0x18) == '\x02') || (*(char *)(iVar9 + 0x18) == '\x05')) {
    if (((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0) ||
       ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0x800)) {
      iVar4 = FUN_0158f010(param_1,iVar9,0,0x8104,0x80);
    }
    else {
      iVar4 = FUN_0158f010(param_1,iVar9,0,0x8104,0x8091);
    }
  }
  else {
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x83c0,0x6000,0x6000);
  }
  if (iVar4 < 0) {
    return iVar4;
  }
  if (((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0) &&
     ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0x800)) {
    if ((*(char *)(iVar9 + 0x18) == '\x02') || (*(char *)(iVar9 + 0x18) == '\x05')) {
      uVar6 = 0;
      if (*(int *)(iVar9 + 0x1fc) != 0) {
        uVar6 = 0x20;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8007,uVar6,0x20);
    }
    else {
      uVar6 = 0;
      if (*(int *)(iVar9 + 0x1fc) != 0) {
        uVar6 = 0x800;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x83c0,uVar6,0x800);
    }
    if (iVar4 < 0) {
      return iVar4;
    }
  }
  iVar4 = FUN_0158b954(param_1,param_2,auStack_58);
  if (iVar4 < 0) {
    return iVar4;
  }
  memcpy((void *)(iVar9 + 0xbc),auStack_58,0x30);
  iVar4 = FUN_01591d20(param_1,param_2,auStack_58);
  if (iVar4 < 0) {
    return iVar4;
  }
  uVar6 = 7;
  if (*(int *)(iVar9 + 0x1ac) == 0) {
    uVar6 = 6;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8301,uVar6,7);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_0158c09c(param_1,param_2,auStack_78,9);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_01594d90(param_1,param_2,auStack_78);
  if (iVar4 < 0) {
    return iVar4;
  }
  uVar5 = 0x50;
  if (*(int *)(iVar9 + 0x19c) == 0) {
    uVar5 = 0x40;
  }
  if (*(int *)(iVar9 + 0x1a0) != 0) {
    uVar5 = uVar5 | 1;
  }
  uVar6 = 0x51;
  if (*(int *)(iVar9 + 0x1a4) != 0) {
    uVar5 = uVar5 | 0x20;
    uVar6 = 0x71;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8300,uVar5,uVar6);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8302,0x2004,0x2006);
  if (iVar4 < 0) {
    return iVar4;
  }
  if ((((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0) ||
      ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0x800)) &&
     (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x810e,7,0x1f), iVar4 < 0)) {
    return iVar4;
  }
  cVar7 = *(char *)(iVar9 + 0x18);
  if ((cVar7 == '\x02') || (cVar7 == '\x05')) {
    *(uint *)(iVar9 + 0x1d4) = 2 - (uint)(*(char *)(iVar9 + 0x17) == '\0');
    iVar4 = FUN_015991d4(param_1,iVar9);
    if (iVar4 < 0) {
      return iVar4;
    }
    cVar7 = *(char *)(iVar9 + 0x18);
    if ((cVar7 != '\x02') && (cVar7 != '\x05')) goto LAB_0159a1f8;
    *(undefined4 *)(iVar9 + 0x1d4) = 0;
LAB_0159a920:
    iVar4 = *(int *)(iVar9 + 0x1bc);
    if ((((iVar4 == 3) || (iVar4 == 5)) || (iVar4 == 4)) || (iVar4 == 6)) {
      iVar4 = FUN_01593058(param_1,param_2);
      if (iVar4 < 0) {
        return iVar4;
      }
      cVar7 = *(char *)(iVar9 + 0x18);
    }
    if (cVar7 != '\x05') goto LAB_0159a208;
  }
  else {
LAB_0159a1f8:
    if (cVar7 == '\x05') goto LAB_0159a920;
LAB_0159a208:
    local_80[0] = 0;
    if (cVar7 != '\x02') {
      uVar3 = *(ushort *)(iVar9 + 0x2a4) & 0xf800;
      if (((((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0) || (uVar3 == 0x800)) ||
          ((uVar3 == 0x4000 || (uVar3 == 0x4800)))) &&
         (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8309,0x20,0x20), iVar4 < 0)) {
        return iVar4;
      }
      if (*(int *)(iVar9 + 0x1c0) == 0) {
        uVar6 = 0;
        uVar3 = 0;
        if (*(int *)(iVar9 + 0x1bc) == 0) {
          iVar4 = FUN_015936ac(param_1,param_2,*(undefined4 *)(iVar9 + 0x1c),local_80,auStack_7c);
          if (iVar4 < 0) {
            return iVar4;
          }
          if ((local_80[0] & 0x20) != 0) {
            uVar6 = 0x80;
          }
          uVar3 = local_80[0] & 0x1f;
        }
      }
      else {
        uVar6 = 0;
        uVar3 = 0;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8308,uVar3,0x1f);
      if (iVar4 < 0) {
        return iVar4;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x833c,uVar6,0x80);
      if (iVar4 < 0) {
        return iVar4;
      }
      uVar3 = *(ushort *)(iVar9 + 0x2a4) & 0xf800;
      if (((((*(ushort *)(iVar9 + 0x2a4) & 0xf800) == 0) || (uVar3 == 0x800)) || (uVar3 == 0x4000))
         || (uVar3 == 0x4800)) {
        if ((*(char *)(iVar9 + 0x18) != '\x02') && (*(char *)(iVar9 + 0x18) != '\x05')) {
          iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8309,0,0x20);
          if (iVar4 < 0) {
            return iVar4;
          }
          goto LAB_0159a2dc;
        }
LAB_0159abc8:
        *(undefined4 *)(iVar9 + 0x1d4) = 0;
      }
      else {
LAB_0159a2dc:
        if ((*(char *)(iVar9 + 0x18) == '\x02') || (*(char *)(iVar9 + 0x18) == '\x05'))
        goto LAB_0159abc8;
      }
      uVar6 = 0x1200;
      if (*(int *)(iVar9 + 0x1c0) == 0) {
        uVar6 = 0;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0xffe0,uVar6,0x1200);
      if (iVar4 < 0) {
        return iVar4;
      }
      if ((*(int *)(iVar9 + 0x294) == 1) || (uVar5 = 0x1200, *(int *)(iVar9 + 0x294) == 3)) {
        uVar5 = 1 << (*(byte *)(iVar9 + 0x17) & 0x3f) & 0xffff;
        iVar4 = FUN_0158fbdc(param_1,iVar9,0,0xffc8,uVar5,uVar5);
        if (iVar4 < 0) {
          return iVar4;
        }
        *(undefined4 *)(iVar9 + 0x294) = 2;
      }
      uVar6 = 0x1200;
      if (*(int *)(iVar9 + 0x1bc) == 0) {
        uVar6 = 0;
      }
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x38000000,uVar6,uVar5);
      if (iVar4 < 0) {
        return iVar4;
      }
      goto LAB_0159a374;
    }
  }
  local_80[0] = 0;
  *(uint *)(iVar9 + 0x1d4) = 2 - (uint)(*(char *)(iVar9 + 0x17) == '\0');
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0xffe0,0,0x1000);
  if (iVar4 < 0) {
    return iVar4;
  }
  if (*(char *)(iVar9 + 0x18) == '\x05') {
    uVar6 = 0x80;
    uVar3 = 0xf;
  }
  else {
    iVar4 = FUN_015936ac(param_1,param_2,*(undefined4 *)(iVar9 + 0x1c),local_80,auStack_7c);
    if (iVar4 < 0) {
      return iVar4;
    }
    uVar6 = 0;
    if ((local_80[0] & 0x20) != 0) {
      uVar6 = 0x80;
    }
    uVar3 = local_80[0] & 0x1f;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8308,uVar3,0x1f);
  if (iVar4 < 0) {
    return iVar4;
  }
  iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x833c,uVar6,0x80);
  if (iVar4 < 0) {
    return iVar4;
  }
  if ((*(char *)(iVar9 + 0x18) == '\x02') || (*(char *)(iVar9 + 0x18) == '\x05')) {
    *(undefined4 *)(iVar9 + 0x1d4) = 0;
  }
LAB_0159a374:
  if (((((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0) &&
       ((*(ushort *)(iVar9 + 0x2a4) & 0xf800) != 0x800)) ||
      (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8111,8,8), -1 < iVar4)) &&
     (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8345,0,0xc000), -1 < iVar4)) {
    if (*(int *)(iVar9 + 0x294) == 3) {
      uVar5 = 1 << (*(byte *)(iVar9 + 0x17) & 0x3f) & 0xffff;
      iVar4 = FUN_0158fbdc(param_1,iVar9,0,0xffc8,uVar5,uVar5);
      if (iVar4 < 0) {
        return iVar4;
      }
      *(undefined4 *)(iVar9 + 0x294) = 2;
    }
    iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x833d,0x8000,0x8000);
    if (((-1 < iVar4) && (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x833e,0xc000,0xc000), -1 < iVar4))
       && ((iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x8150,7,7), -1 < iVar4 &&
           (iVar4 = FUN_0158fbdc(param_1,iVar9,0,0x832b,0,2), -1 < iVar4)))) {
      iVar9 = FUN_01393e80(0xa00fe04);
      iVar4 = 0;
      if (iVar9 != 0) {
        uVar6 = *(undefined4 *)(iVar1 + 0x1591e44);
        if (iVar8 != 0) {
          uVar6 = *(undefined4 *)(iVar1 + 0x1591e40);
        }
        FUN_013949c0(*(undefined4 *)(iVar1 + 0x1591e4c),0xa00fe04,*(undefined4 *)(iVar1 + 0x1591d2c)
                     ,0x9d1,*(int *)(iVar1 + 0x1591d48) + 0x354,param_1,param_1,param_2,uVar6);
        iVar4 = 0;
      }
    }
  }
  return iVar4;
}


