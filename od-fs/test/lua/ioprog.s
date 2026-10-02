; A program for test_harddrive.py, built with vasm and vlink. It reads a file
; from the disk it was started from, over and over, and counts the times in
; counter. This keeps the file system of the disk busy all the time.

        section code,code

start:
        move.l  4.w,a6
        lea     dosname(pc),a1
        moveq   #0,d0
        jsr     -552(a6)                ; OpenLibrary
        move.l  d0,a6
loop:
        move.l  #filename,d1
        move.l  #1005,d2                ; MODE_OLDFILE
        jsr     -30(a6)                 ; Open
        move.l  d0,d4
        beq.s   loop
        move.l  d4,d1
        move.l  #buffer,d2
        move.l  #512,d3
        jsr     -42(a6)                 ; Read
        move.l  d4,d1
        jsr     -36(a6)                 ; Close
        addq.l  #1,counter
        bra.s   loop

dosname:
        dc.b    "dos.library",0
filename:
        dc.b    "s/startup-sequence",0

        section data,data

counter:
        dc.l    0
buffer:
        ds.b    512
