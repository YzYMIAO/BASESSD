
	

.global _start
.text

_start:
	ldr x0,=array
	mov x1,#10
	mov x2,#0
	mov x3,#0
	mov x4,#0
	mov x5,#0
	mov x6,#0

.loop:
	cmp x6,x1
	b.ge .loop_end

	ldr w7,[x0,x6,lsl #2]
	
	cmp w7,#0
	b.gt .positive
	b.lt .negative
	
.zero:
	add x5,x5,#1
	b .next

.positive:
	add x2,x2,x7
	add x3,x3,#1
	b .next

.negative:
	add x4,x4,#1
	b .next

.next:
	add x6,x6,#1
	b .loop

.loop_end:
	mov x0,x2
	mov x8,#93
	svc #0
	
	
.data
array:
	.word 5,-3,0,-8,-1,0,-6,-4,2,0

	