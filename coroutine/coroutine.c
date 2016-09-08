#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
#include <sys/ucontext.h>
#else 
#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024) // ��ʾջ�Ĵ�С
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
	char stack[STACK_SIZE]; // ԭ��schedule������Ѿ�������stack
	ucontext_t main; // ucontext_t����Կ����Ǽ�¼��������Ϣ��һ���ṹ
	int nco; // Э�̵���Ŀ
	int cap; // ����
	int running; // �������е�coroutine��id
	struct coroutine **co; // ������һ����ά��ָ��
};

struct coroutine {
	coroutine_func func; // ���еĺ���
	void *ud; // ����
	ucontext_t ctx; // ���ڼ�¼��������Ϣ��һ���ṹ
	struct schedule * sch; // ָ��schedule
	ptrdiff_t cap; // ��ջ������
	ptrdiff_t size; // ���ڱ�ʾ��ջ�Ĵ�С
	int status;
	char *stack; // ָ��ջ��ַô��
};

struct coroutine *
	_co_new(struct schedule *S, coroutine_func func, void *ud) { // ���ڹ���һ��coroutine��
	struct coroutine * co = malloc(sizeof(*co)); // �¹���һ��coroutine�ṹ
	co->func = func; // ��¼Ҫ���еĺ���
	co->ud = ud; // ����
	co->sch = S; // schedule������������ʲô�ģ�һ��ָ������ָ��S
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL; // Ҫ����������ָ�����NULL
	return co;
}

void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}

struct schedule *
	coroutine_open(void) { // ���ڴ�һ��coroutine�ǰɣ�
	struct schedule *S = malloc(sizeof(*S)); // ��Ȼ����һ��schedule
	S->nco = 0; // ��ʾЭ�̵���ĿΪ0
	S->cap = DEFAULT_COROUTINE; // DEFAULT_COROUTINEò����16���� cap��ʾ����
	S->running = -1; // -1��ʾ��û�п�ʼ����
	S->co = malloc(sizeof(struct coroutine *) * S->cap); 
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

void
coroutine_close(struct schedule *S) {
	int i;
	for (i = 0; i < S->cap; i++) {
		struct coroutine * co = S->co[i];
		if (co) { // �����Ϊ��
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

int
coroutine_new(struct schedule *S, coroutine_func func, void *ud) { // �����½�һ��һ��coroutine
	struct coroutine *co = _co_new(S, func, ud); // ud�������Ϊ�ǲ���
	if (S->nco >= S->cap) { // nco��ʾЭ�̵���Ŀ���������ˡ�
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *)); // �������·���
		memset(S->co + S->cap, 0, sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co; // �ðɣ����ڿ�ʼװ��coroutine�ˣ�
		S->cap *= 2; // ��Ϊ������������
		++S->nco;
		return id;
	}
	else {
		int i;
		for (i = 0; i < S->cap; i++) { // ��֮���ǲ���Ѱ�ң��ҵ�һ���յ�λ��Ϊֹ
			int id = (i + S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id]; // �õ���Ӧ��coroutine
	C->func(S, C->ud); // ���ж�Ӧ�ĺ���, ud���û�����Ĳ���
	_co_delete(C); // �����������ˣ���ɾ�����coroutine
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

void
coroutine_resume(struct schedule * S, int id) { // ���ڻָ�coroutine������
	assert(S->running == -1);
	assert(id >= 0 && id < S->cap);
	struct coroutine *C = S->co[id]; // �õ���Ӧ��Э��
	if (C == NULL)
		return;
	int status = C->status;
	switch (status) {
	case COROUTINE_READY: // ����ǲ�׼���ÿ�ʼִ��
		getcontext(&C->ctx); // ��ʼ��һ��ctx
		C->ctx.uc_stack.ss_sp = S->stack; // �ðɣ�����schedule��stackΪcoroutine�Ķ�ջ����Ϊ�㹻��
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		C->ctx.uc_link = &S->main; // coroutine�������֮��Ĭ�ϻص�scheduleָ���main context����ִ��
		S->running = id; // running��ʾ�����������е�coroutine��id
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S; 
		// mainfunc������ļ����һ������
		makecontext(&C->ctx, (void(*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr >> 32));
		swapcontext(&S->main, &C->ctx); // �����л�������
		break;
	case COROUTINE_SUSPEND: // �����suspend�лָ�
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size); // �Ȼָ���ջ��Ϣ
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx); // �л�������
		break;
	default:
		assert(0);
	}
}

static void
_save_stack(struct coroutine *C, char *top) { // ����������ڱ����ջ����Ϣ
	// ��Ҫע����ǣ�ջ�ǴӸ�λ���λ����
	// ���ԣ�top��ʾ������ʵ�Ƕ�ջ�ĵײ�����Ȼ���ĵ�ַ���
	char dummy = 0; // �����ĺ�����˼��dummy��λ��һ����ջջ�Ķ�������Ϊ����������Ȼ��coroutine�Ķ�ջ��
	assert(top - &dummy <= STACK_SIZE); // top - &dummyǡ����ջ�Ĵ�С
	if (C->cap < top - &dummy) { // ��һ�α����ʱ��,C->capΪ0,C->stackΪNULL
		// ע������C->stack��C->ctk->uc_stack.ss_sp�������߲���ͬһ��������ǰ���C->ctk->uc_stack.ss_sp���ó���schedule��stack
		free(C->stack); // �����ջ�����c->stackΪNULL����ôfreeʲôҲ���ɣ����ǹ涨
		C->cap = top - &dummy;
		C->stack = malloc(C->cap); // ���������malloc
	}
	C->size = top - &dummy;
	// Ҳ����˵&dummy�ڵ�λ
	memcpy(C->stack, &dummy, C->size); // ��ʵ�������C->stack����˼�ǰɣ�
}

void
coroutine_yield(struct schedule * S) {
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C, S->stack + STACK_SIZE); // ���ڱ����ջ����Ϣ
	C->status = COROUTINE_SUSPEND; // ״̬����˹���
	S->running = -1;
	swapcontext(&C->ctx, &S->main);
}

int
coroutine_status(struct schedule * S, int id) {
	assert(id >= 0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD; // COROUTINE_DEAD����0
	}
	return S->co[id]->status;
}

int
coroutine_running(struct schedule * S) {
	return S->running;
}

