# RUN: llvm-mc -triple=ve --show-encoding < %s \
# RUN:     | FileCheck %s --check-prefixes=CHECK-ENCODING,CHECK-INST
# RUN: llvm-mc -triple=ve -filetype=obj < %s | llvm-objdump -d - \
# RUN:     | FileCheck %s --check-prefixes=CHECK-INST

# CHECK-INST: vfmk.w.at %vm11
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x0b,0x00,0x00,0x00,0xb5]
vfmk.w %vm11

# CHECK-INST: vfmk.w.at %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x01,0x00,0x00,0x0f,0xb5]
vfmk.w.at %vm1, %vm15

# CHECK-INST: vfmk.w.af %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x01,0x00,0x00,0x0f,0xb5]
vfmk.w.af %vm1, %vm15

# CHECK-INST: vfmk.w.gt %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x01,0x0c,0x00,0x00,0x00,0xb5]
vfmk.w.gt %vm12, %v22

# CHECK-INST: vfmk.w.lt %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x02,0x0c,0x00,0x00,0x0f,0xb5]
vfmk.w.lt %vm12, %vix, %vm15

# CHECK-INST: vfmk.w.ne %vm11, %v32
# CHECK-ENCODING: encoding: [0x00,0x20,0x03,0x0b,0x00,0x00,0x00,0xb5]
vfmk.w.ne %vm11, %v32

# CHECK-INST: vfmk.w.eq %vm1, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x04,0x01,0x00,0x00,0x0f,0xb5]
vfmk.w.eq %vm1, %vix, %vm15

# CHECK-INST: vfmk.w.ge %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x05,0x0c,0x00,0x00,0x00,0xb5]
vfmk.w.ge %vm12, %v22

# CHECK-INST: vfmk.w.le %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x06,0x0c,0x00,0x00,0x0f,0xb5]
vfmk.w.le %vm12, %vix, %vm15

# CHECK-INST: vfmk.w.at %vm11
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x0b,0x00,0x00,0x00,0xb5]
pvfmk.w.lo %vm11

# CHECK-INST: vfmk.w.at %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x01,0x00,0x00,0x0f,0xb5]
pvfmk.w.lo.at %vm1, %vm15

# CHECK-INST: vfmk.w.af %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x01,0x00,0x00,0x0f,0xb5]
pvfmk.w.lo.af %vm1, %vm15

# CHECK-INST: vfmk.w.gt %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x01,0x0c,0x00,0x00,0x00,0xb5]
pvfmk.w.lo.gt %vm12, %v22

# CHECK-INST: vfmk.w.lt %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x02,0x0c,0x00,0x00,0x0f,0xb5]
pvfmk.w.lo.lt %vm12, %vix, %vm15

# CHECK-INST: vfmk.w.ne %vm11, %v32
# CHECK-ENCODING: encoding: [0x00,0x20,0x03,0x0b,0x00,0x00,0x00,0xb5]
pvfmk.w.lo.ne %vm11, %v32

# CHECK-INST: vfmk.w.eq %vm1, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x04,0x01,0x00,0x00,0x0f,0xb5]
pvfmk.w.lo.eq %vm1, %vix, %vm15

# CHECK-INST: vfmk.w.ge %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x05,0x0c,0x00,0x00,0x00,0xb5]
pvfmk.w.lo.ge %vm12, %v22

# CHECK-INST: vfmk.w.le %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x06,0x0c,0x00,0x00,0x0f,0xb5]
pvfmk.w.lo.le %vm12, %vix, %vm15

# CHECK-INST: pvfmk.w.up.at %vm11
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x0b,0x00,0x00,0x80,0xb5]
pvfmk.w.up %vm11

# CHECK-INST: pvfmk.w.up.at %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x0f,0x01,0x00,0x00,0x8f,0xb5]
pvfmk.w.up.at %vm1, %vm15

# CHECK-INST: pvfmk.w.up.af %vm1, %vm15
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x01,0x00,0x00,0x8f,0xb5]
pvfmk.w.up.af %vm1, %vm15

# CHECK-INST: pvfmk.w.up.gt %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x01,0x0c,0x00,0x00,0x80,0xb5]
pvfmk.w.up.gt %vm12, %v22

# CHECK-INST: pvfmk.w.up.lt %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x02,0x0c,0x00,0x00,0x8f,0xb5]
pvfmk.w.up.lt %vm12, %vix, %vm15

# CHECK-INST: pvfmk.w.up.ne %vm11, %v32
# CHECK-ENCODING: encoding: [0x00,0x20,0x03,0x0b,0x00,0x00,0x80,0xb5]
pvfmk.w.up.ne %vm11, %v32

# CHECK-INST: pvfmk.w.up.eq %vm1, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x04,0x01,0x00,0x00,0x8f,0xb5]
pvfmk.w.up.eq %vm1, %vix, %vm15

# CHECK-INST: pvfmk.w.up.ge %vm12, %v22
# CHECK-ENCODING: encoding: [0x00,0x16,0x05,0x0c,0x00,0x00,0x80,0xb5]
pvfmk.w.up.ge %vm12, %v22

# CHECK-INST: pvfmk.w.up.le %vm12, %vix, %vm15
# CHECK-ENCODING: encoding: [0x00,0xff,0x06,0x0c,0x00,0x00,0x8f,0xb5]
pvfmk.w.up.le %vm12, %vix, %vm15