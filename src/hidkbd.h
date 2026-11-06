#ifndef _HIDKBD_H_
#define _HIDKBD_H_

/* HID_KEYBOARD=1 (cmake -DHID_KEYBOARD=ON) で USB キーボードを MSC に足す。既定は外す */
#ifndef HID_KEYBOARD
#define HID_KEYBOARD 0
#endif
/* HID_MOUSE=1 (cmake -DHID_MOUSE=ON、HID_KEYBOARD のときだけ) で USB マウスも足す (HID のインスタンス 1、src/hidmouse_cmd.c) */
#if !HID_KEYBOARD
#undef HID_MOUSE
#endif
#ifndef HID_MOUSE
#define HID_MOUSE 0
#endif
#define HIDKBD_INST     0               /* tud_hid_n_* のインスタンス番号 (usb_descriptors.c の HID の並び順) */
#define HIDMOUSE_INST   1
#if HID_KEYBOARD
void hidkbd_init(void);                 /* UDP 6869 の口と status のタスクを作る。cyw43_arch_init のあとで */
void hidkbd_task(void);                 /* USB タスクから毎周呼ぶ。待たない */
#else
#define hidkbd_init()   ((void)0)
#define hidkbd_task()   ((void)0)
#endif

#endif
