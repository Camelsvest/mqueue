#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <errno.h>
#include <windows.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include "_mqueue.h"

#define SIZE_TO_WLEN(s)	((s) / sizeof(wchar_t) - 1)
#define MQ_MSG_ALIVE	0x1
#define MQ_MQD_ALIVE	0x1
#define MQ_PREFIX	L"/mq/"
#define MQ_LOCK_SUFFIX	L".lock"
#define MQ_FULL_EVENT_SUFFIX	L".1"
#define MQ_EMPTY_EVENT_SUFFIX	L".0"
#define MQ_LOCK_LEN	(SIZE_TO_WLEN(sizeof(MQ_LOCK_SUFFIX)))
#define MQ_PREFIX_LEN	(SIZE_TO_WLEN(sizeof(MQ_PREFIX)))
#define MQ_NAME_MAX	((MAX_PATH - MQ_LOCK_LEN) - MQ_PREFIX_LEN)

static struct mqdtable *mqdtab;

static void mqd_destroy_shared_lock(struct mqd *d)
{
	CloseHandle(d->mutex);
}

static void mqd_destroy_local_lock(struct mqd *d)
{
	void *mutex = d->mutex;
	DeleteCriticalSection(mutex);
}

static void mqd_destroy_lock(struct mqd *d)
{
	if (d->flags & O_PRIVATE) mqd_destroy_local_lock(d);
	else mqd_destroy_shared_lock(d);
}

static void mqdtable_lock(void)
{
	EnterCriticalSection(&mqdtab->lock);
}

static void mqdtable_unlock(void)
{
	LeaveCriticalSection(&mqdtab->lock);
}

static volatile struct mqd *get_mqd(mqd_t mqdes)
{
	if (mqdes < 0 || mqdes >= MQ_OPEN_MAX)
		goto bad;

	if (mqdtab->desc[mqdes].eflags & MQ_MQD_ALIVE)
		return &mqdtab->desc[mqdes];

bad:
	errno = EBADF;
	return NULL;
}

static int mqd_wait_handle(HANDLE h)
{
	switch (WaitForSingleObject(h, INFINITE)) {
	case WAIT_ABANDONED:
		errno = EBADMSG;
		return -1;
	case WAIT_OBJECT_0:
		return 0;
	}
	errno = EOTHER;
	return -1;
}


static void mqd_release_shared_lock(volatile struct mqd *d)
{
	ReleaseMutex((void*)d->mutex);
}


static void mqd_get_local_lock(volatile struct mqd *d)
{
	EnterCriticalSection((void*)&d->private_lock);
}

static int mqd_get_lock(volatile struct mqd *d)
{
	if (!(d->flags & O_PRIVATE) && mqd_wait_handle(d->mutex)) {
		return -1;
	}

	if (d->flags & O_PRIVATE)
		mqd_get_local_lock(d);

	return 0;
}

static volatile struct mqd *get_and_lock_mqd(mqd_t mqdes)
{
	/* assume that mqdtab is never NULL */
	volatile struct mqd *res;

	mqdtable_lock();

	res = get_mqd(mqdes);

	if(res != NULL)
		mqd_get_lock(res);

	mqdtable_unlock();

	return res;
}

static void mqd_release_local_lock(volatile struct mqd *d)
{
	LeaveCriticalSection((void*)&d->private_lock);
}

static void mqd_release_lock(volatile struct mqd *d)
{
	if (d->flags & O_PRIVATE)
		mqd_release_shared_lock(d);
	else
		DeleteCriticalSection(d->mutex);
}

static void mqd_create_local_lock(volatile struct mqd *d)
{
	InitializeCriticalSection((void*)&d->private_lock);
}

/* ASSUME: descriptor is for a public queue. */
static int mqd_create_and_get_lock(struct mqd *d, wchar_t *name)
{
	int err;
	int flags = d->flags;

	if (flags & O_CREAT) {
		d->mutex = CreateMutexW(NULL, TRUE, name);
		err = GetLastError();

		if (err == ERROR_ALREADY_EXISTS) {
			errno = EEXIST;
			/* FAIL only if O_EXCL is set */
			if (d->flags & O_EXCL)
				return -1;

			/* Signal the mutex. Errno will get changed on
			 * failure which is OK.
			 */
			mqd_wait_handle(d);
			return -1;
		}

		/* mutex probably exists, so try to get a handle */
		if (err == ERROR_ACCESS_DENIED)
			goto open_mutex;

		/* A mutex was opened and locked! */
		if (d->mutex != NULL)
			return 0;

	} else {
open_mutex:
		d->mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);

		/*
		 * If a handle was obtained, then return the value.
		 * FIXME: handle wait failure.
		 */
		if (d->mutex != NULL)
			return mqd_wait_handle(d);

		/* mutex is null, check last and set ENOENT, if the
		 * mutex does not exist.
		 */
		err = GetLastError();
		if (err == ERROR_NOT_FOUND) {
			errno = ENOENT;
			return -1;
		}
	}

	/* No idea what error codes lead to this */
	SetLastError(err);
	errno = EOTHER;
	return -1;
}

#if 0
/* XXX: How to set gid underwindows? */
void create_secdesc(mode_t mode)
{
	static PSID oth;
	SID_IDENTIFIER_AUTHORIDY oauth = SECURITY_WORLD_SID_AUTHORITY;
	EXPLICIT_ACCESS_W acc[3];
	DWORD size;

	memset(acc, 0, sizeof(acc));

	size = SECURITY_MAX_SID_SIZE;

	if (oth == NULL)
		oth = LocalAlloc(LMEM_FIXED, size);

	/* XXX: what happens if NULL is passed instead of size? */
	CreateWellKnownSid(WinWorldSid, NULL, &oth, &size);

	/* check S_IRshits */
	if (!mode & S_IROTH)
		acc[0].grfAccessPermission |= READ_CONTROL;
	if (!mode & S_IWOTH)
		acc[0].grfAccessPermission |= WRITE_CONTROL;

	/* deny write access */
	acc[0].grfAccessPermission |= EXECUTE_CONTROL;
	acc[0].grfAccessMode = DENY_ACCESS;
	acc[0].grfInheritance = NO_INHERITANCE;
	acc[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	acc[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	acc[0].Trustee.ptrstrName = oth;
}
#endif

WCHAR *inflate(const char *src, DWORD maxsize)
{
	WCHAR *tmp;
	DWORD tmpsize;

	tmpsize = MultiByteToWideChar(CP_THREAD_ACP,
				      MB_ERR_INVALID_CHARS,
				      src,
				      -1,
				      NULL,
				      0);

	if (tmpsize > maxsize)
		return NULL;

	tmp = malloc(tmpsize);

	MultiByteToWideChar(CP_THREAD_ACP,
			    MB_ERR_INVALID_CHARS,
			    src,
			    -1,
			    tmp,
			    tmpsize);

	return tmp;
}

static struct message *mqueue_get_msg(volatile struct mqueue *mq, int index)
{
	if (index < 0 || index >= mq->maxmsg)
		return NULL;
	return (struct message*)&((char*)(mq->buffer))[index * mq->msgsize];
}

static int valid_open_attr(struct mq_attr *a)
{
	return a->mq_maxmsg > 0 && a->mq_msgsize > 0 && a->mq_sleepdur > 0;
}

static void mqd_destroy_shared_cond(struct mqd *d)
{
	int i = 0;
	for (; i < MQ_PRIO_MAX + 1; ++i) {
		/* threads waiting for a message slot */
		SetEvent(&d->cond[MQ_PRIO_MAX][0].shared);
		CloseHandle(&d->cond[MQ_PRIO_MAX][0].shared);
		d->cond[i][0].shared = NULL;

		/* threads waiting for a message */
		SetEvent(&d->cond[i][1].shared);
		CloseHandle(&d->cond[i][1].shared);
		d->cond[i][1].shared = NULL;
	}
}

static HANDLE do_mqd_create_shared_cond(struct mqd *d, wchar_t *name,
	DWORD *err)
{
	HANDLE v = CreateEventW(NULL, FALSE, TRUE, name);

	*err = GetLastError();
	if (*err == ERROR_ALREADY_EXISTS) {
		if (d->flags & O_EXCL) {
			CloseHandle(v);
			v = NULL;
		}
		errno = EEXIST;
		d->flags &= ~O_CREAT;
	} else if (v == NULL) {
		errno = EOTHER;
	}

	return v;
}

static int mqd_create_shared_cond(struct mqd *d, wchar_t *empty,
	wchar_t *full)
{
	DWORD err;
	int i, j;

	for (i = 0; i <= MQ_PRIO_MAX; ++i) {
		d->cond[i][0].shared = do_mqd_create_shared_cond(d, empty, &err);
		if (d->cond[i][0].shared == NULL) {
			goto bad;
		}
	}

	for (j = 0; j <= MQ_PRIO_MAX; ++j) {
		d->cond[j][1].shared = do_mqd_create_shared_cond(d, full, &err);

		if (d->cond[j][1].shared == NULL) {
			for (--j; j >= 0; --j)
				CloseHandle(d->cond[j][0].shared);
		bad:
			for (--i; i >= 0; --i)
				CloseHandle(d->cond[j][0].shared);

			SetLastError(err);
			return -1;
		}
	}

	return 0;
}

static void mqd_create_local_cond(struct mqd *d)
{
	int i, j;
	for (i = 0; i <= MQ_PRIO_MAX; ++i) {
		d->cond[i][0].local = malloc(sizeof(CONDITION_VARIABLE));
		InitializeConditionVariable(d->cond[i][0].local);
	}
	for (j = 0; j <= MQ_PRIO_MAX; ++j) {
		d->cond[j][1].local = malloc(sizeof(CONDITION_VARIABLE));
		InitializeConditionVariable(d->cond[j][0].local);
	}
}

static void mqd_destroy_local_cond(struct mqd *d)
{
	int i = 0;
	for (; i >= 0; --i) {
		free(d->cond[i][0].local);
		free(d->cond[i][1].local);
		d->cond[i][0].local = NULL;
		d->cond[i][1].local = NULL;
	}
}

static int mqd_create_cond(struct mqd *d, wchar_t *empty, wchar_t *full)
{
	if (d->flags & O_PRIVATE) {
		mqd_create_local_cond(d);
		return 0;
	}
	return mqd_create_shared_cond(d, empty, full);
}

static void mqd_destroy_cond(struct mqd *d)
{
	if (d->flags & O_PRIVATE)
		mqd_destroy_local_cond(d);
	else
		mqd_destroy_shared_cond(d);
}

mqd_t mq_open(const char *name, int oflag, ...)
{
	va_list al;
	struct mqd d;
	volatile struct mqd *qd;
	struct mqueue *mq;
	HANDLE *map;
	wchar_t *mqname, *lkname, *wname, *cvfull, *cvempty;
	/* TODO: do not use this many buffers. */
	wchar_t lkname_[MAX_PATH], mqname_[MAX_PATH];
	wchar_t cvfull_[MAX_PATH], cvempty_[MAX_PATH];
	DWORD err, mapacc;
	int res, private;

	private = oflag & O_PRIVATE;

	mqdtable_lock();

	if ((qd = mqdtab->free_mqd.head) == NULL) {
		errno = EMFILE;
		goto close_map;
	}

	/* convert the name to unicode */
	wname = inflate(name, MQ_NAME_MAX);
	if (wname == NULL) {
		errno = ENAMETOOLONG;
		return -1;
	}

	wcscpy(mqname_, MQ_PREFIX);
	wcscat(mqname_, wname);

	if (private) {
		lkname = mqname = NULL;
	} else {
		wcscpy(lkname_, mqname_);
		wcscat(lkname_, MQ_LOCK_SUFFIX);
		wcscpy(cvfull_, mqname_);
		wcscat(cvfull_, MQ_FULL_EVENT_SUFFIX);
		wcscpy(cvempty_, mqname_);
		wcscat(cvfull_, MQ_EMPTY_EVENT_SUFFIX);

		lkname = lkname_;
		mqname = mqname_;
		cvfull = cvfull_;
		cvempty = cvempty_;
	}

	free(wname);
	memset(&d, 0, sizeof(d));
	d.flags = oflag;

	/* create the muteces */
	if(mqd_create_and_get_lock(&d, lkname)) {
		if ((errno == EEXIST && oflag & O_EXCL) || errno == EOTHER)
			return -1;
	}

	/* create condition variables. */
	if (mqd_create_cond(&d, cvempty, cvfull)) {
		if ((errno == EEXIST && oflag & O_EXCL) || errno == EOTHER)
			return -1;
	}

	mapacc = 0;
	switch (oflag & (O_RDWR | O_WRONLY)) {
	case O_RDWR:
		mapacc = FILE_MAP_READ | FILE_MAP_WRITE;
		break;
	case O_WRONLY:
		mapacc = FILE_MAP_WRITE;
		break;
	case O_RDONLY:
		mapacc = FILE_MAP_READ;
		break;
	default:
		errno = EINVAL;
		goto destroy_cond;
	}

	/* map the page file */
	if (oflag & O_CREAT) {
		struct mq_attr *attr;
		DWORD mapsize;
		int mode;
		int i;

		va_start(al, oflag);
		mode = va_arg(al, int);
		attr = va_arg(al, struct mq_attr *);

		/* secdesc = create_secdesc(mode); */

		if (attr == NULL) {
			d.attr.mq_maxmsg = MQ_MAX_MSG;
			d.attr.mq_msgsize = MQ_MSG_SIZE;
		} else if (valid_open_attr(attr)) {
			d.attr = *attr;
		} else {
			errno = EINVAL;
			goto destroy_lock;
		}

		/* Calculate the required size for the queue */
		d.attr.mq_msgsize = (sizeof(struct message)
					+ d.attr.mq_msgsize - 1);
		d.attr.mq_msgsize += d.attr.mq_msgsize % sizeof(int);
		mapsize = (sizeof(struct mqueue) - sizeof(struct message))
				+ (d.attr.mq_msgsize * d.attr.mq_maxmsg);
		mapsize += mapsize % sizeof(int);

		map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
					 PAGE_READWRITE, 0, mapsize, mqname);

		err = GetLastError();

		if (map == NULL) {
			/* let the application handle failure :) */
			SetLastError(err);
			errno = EOTHER;
			goto destroy_lock;
		}

		if (err == ERROR_ALREADY_EXISTS) {
			if (oflag & O_EXCL) {
				errno = EEXIST;
				goto close_map;
			}
			goto copy_open;
		}

		mq = MapViewOfFile(map, mapacc, 0, 0, 0);

		if (mq == NULL) {
			errno = EOTHER;
			goto close_map;
		}

		mq->maxmsg = d.attr.mq_maxmsg;
		mq->msgsize = d.attr.mq_curmsg;
		memcpy(mq->name, mqname, wcslen(mqname));
		mq->free_tail = d.attr.mq_maxmsg - 1;

		for (i = 1; i < d.attr.mq_maxmsg; ++i) {
			mqueue_get_msg(mq, i - 1)->next = i;
			mqueue_get_msg(mq, i)->prev = i - 1;
		}

		for (i = 0; i < MQ_PRIO_MAX; ++i)
			mq->prio_head[i] = mq->prio_tail[i] = -1;

		mqueue_get_msg(mq, d.attr.mq_maxmsg - 1)->next = -1;
	} else {
		map = OpenFileMappingW(mapacc, 0, mqname);

		if (map == NULL) {
			errno = ENOENT;
			goto destroy_lock;
		}
copy_open:
		mq = MapViewOfFile(map, mapacc, 0, 0, 0);
		if (mq == NULL) {
			errno = EOTHER;
			goto close_map;
		}
		d.attr.mq_curmsg = mq->curmsg;
		d.attr.mq_maxmsg = mq->maxmsg;
		d.attr.mq_msgsize = mq->msgsize;
	}
	d.map = map;
	d.queue = mq;
	d.flags = oflag;
	d.eflags = MQ_MQD_ALIVE;

	/* Since the table is locked here, it is not necessary to lock the
	 * other queue descriptors
	 */
	if (qd->next) qd->next->prev = NULL;
	else mqdtab->free_mqd.tail = NULL;
	mqdtab->free_mqd.head = qd->next;

	qd->prev = mqdtab->live_mqd.tail;
	if (qd->prev) qd->prev->next = (void*)qd;
	else mqdtab->live_mqd.tail = mqdtab->live_mqd.head = (void*)qd;

	qd->next = NULL;
	*qd = d;

	if (d.flags & O_CREAT) {
		int i = 0;

		/*
		 * Signal that the queue is not full. Although no threads
		 * should be waiting on the condition variable, it is safer
		 * to set them here. Infact, no thread would be able to wait
		 * on the variable since the queue is locked.
		 */
		for (; i <= MQ_PRIO_MAX; ++i)
			mq_cond_set(&d.cond[i][0]);
	}

	mqd_release_lock(&d);

	if (0) {
close_map:
		CloseHandle(map);
destroy_cond:
		mqd_destroy_local_cond(&d);
destroy_lock:
		mqd_destroy_lock(&d);
		res = -1;
	}

	mqdtable_unlock();

	return res;
}

int mq_close(mqd_t mqdes)
{
	volatile struct mqd *d;
	int err;

	/* lock table first */
	mqdtable_lock();
	d = get_mqd(mqdes);

	if (d == NULL) {
		errno = EBADF;
		err = -1;
		goto out;
	}

	/*
	 * Wait for other threads to finish up on the queue. No need to own the
	 * public lock as the queue itself is not altered. No need to unlock
	 * the private lock.
	 */
	mqd_get_local_lock(d);
	d->flags &= ~MQ_MQD_ALIVE;

	/* free resources */
	UnmapViewOfFile((LPVOID)d->queue);
	CloseHandle(d->map);
	if(!(d->flags & O_PRIVATE))
		CloseHandle(d->mutex);

	if (d->next)
		d->next->prev = d->prev;

	if (d->prev)
		d->prev->next = d->next;

	/*
	 * Not owning a lock is safe here, as the free list is only accessed
	 * after the table's lock is owned.
	 */
	if (mqdtab->free_mqd.tail) {
		mqdtab->free_mqd.tail->next = (void*)d;
		d->prev = mqdtab->free_mqd.tail;
		mqdtab->free_mqd.tail = (void*)d;
		d->next = NULL;
	} else {
		mqdtab->free_mqd.tail = mqdtab->free_mqd.head = (void*)d;
		d->next = d->prev = NULL;
	}

	/* finally destroy the queue's lock and release the table's lock */
	mqd_destroy_lock(d);
	err = 0;
out:
	LeaveCriticalSection(&mqdtab->lock);
	return err;
}

#if 0
DWORD mqd_find_next_msg(struct mqd *d)
{
	int i;
	DWORD j;
	struct mqueue *q;
	struct message *m;
	long curmsg, maxmsg;

	/* queue is full */
	q = (void *)d->mqd_u.queue;
	curmsg = q->curmsg;
	maxmsg = q->maxmsg;

	if (curmsg >= maxmsg)
		return -1;

	/* an empty queue */
	if (!curmsg)
		return 0;

	/* assume free_head contains some message */
	j = q->free_head;
	m = mqueue_get_msg(q, j);

	/* free_head is a free msg. This will usually be the case except
	 * right after a message has been sent/inserted into the queue.
	 * This function is called to update mqueue->free_head.
	 */
	if (!(m->flags & MQ_MSG_ALIVE))
		return j;

	/* FIXME: only walk the free list */
	for (i = 1, ++j; i < maxmsg; ++i) {
		if (!(mqueue_get_msg(q, j)->flags & MQ_MSG_ALIVE))
			return j;
		j = (j + 1) % (maxmsg - 1);
	}
	/* on the off chance something goes wrong */
	return -1;
}
#endif

int
mq_receive(mqd_t des, char *msg_ptr, size_t msg_size, unsigned *msg_prio)
{
	volatile struct mqd *d;
	volatile struct mqueue *q;
	struct message *m, *next, *tail;
	int err, prio, startprio, minprio, nonblock;

	if (msg_ptr == NULL) {
		errno = EINVAL;
		return -1;
	}

	d = get_and_lock_mqd(des);

	if (d == NULL) {
		errno = EBADF;
		return -1;
	}

	if (!(d->flags & O_RDWR) && d->flags & O_WRONLY) {
		errno = EPERM;
		return -1;
	}

	q = d->queue;

	if (msg_prio != NULL) {
		minprio = startprio = *msg_prio;
		if (minprio >= MQ_PRIO_MAX && minprio < 0) {
			errno = EINVAL;
			goto bad;
		}
	} else {
		minprio = 0;
		startprio = MQ_PRIO_MAX - 1;
	}

	for (prio = startprio; prio >= minprio; --prio) {
		if (q->prio_head[prio] != -1) {
			m = mqueue_get_msg(q, q->prio_head[prio]);
			goto received;
		}
	}

	/* no message was recieved. */
	if (d->attr.mq_flags & O_NONBLOCK) {
		errno = EAGAIN;
		goto bad;
	} else do {
		mqd_release_lock(d);

		/* prio should be valid at this point. */
		if (msg_prio) {
			mq_cond_wait(&d->cond[*msg_prio][1]);
		} else {
			mq_cond_wait(&d->cond[MQ_PRIO_MAX][1]);
		}

		/* Check if the descriptor is still valid. */
		d = get_and_lock_mqd(des);
		if(d == NULL)
			return -1;

		/* It shouldn't fail at this point. */
		for (prio = startprio; prio >= minprio; --prio) {
			m = mqueue_get_msg(d->queue, d->queue->prio_head[prio]);
			if (m != NULL)
				goto received;
		}
		/* loop if failed, and O_NONBLOCK wasn't  set for the
		 * descriptor by another thread.
		 */
	} while (!(d->attr.mq_flags & O_NONBLOCK));

	/* O_NONBLOCK was set, and no message was recieved. */
	errno = EAGAIN;
	return -1;

received:
	if (m->size > msg_size) {
		errno = EMSGSIZE;
		goto bad;
	}

	memcpy(msg_ptr, m->buffer, m->size);

	tail = mqueue_get_msg(q, q->free_tail);
	next = mqueue_get_msg(q, m->next);
	m->flags &= ~MQ_MSG_ALIVE;

	/* add to the free list */
	if (tail) {
		tail->next = q->prio_head[prio];
		m->prev = q->free_tail;
		q->free_tail = q->prio_head[prio];
	} else {
		m->prev = -1;
		m->next = -1;
		q->free_tail = q->prio_head[prio];
		q->free_head = q->prio_head[prio];
	}

	/* remove from the live list */
	if (next) {
		next->prev = -1;
		q->prio_head[prio] = m->next;
	} else {
		int i, empty = 1;

		mq_cond_unset(d->not_empty_prio[prio]);
		q->prio_head[prio] = -1;
		q->prio_tail[prio] = -1;

		for (i = 0; i < MQ_PRIO_MAX; ++i) {
			if (q->prio_head[prio] != -1)
				empty = 0;
		}

		if (empty)
			mq_cond_unset(d->not_empty);

	}

	if (q->curmsg == q->maxmsg - 1) {
		mq_cond_set(d->not_full);
	}

	err = 0;

	if (0) {
bad:
		err = -1;
	}

	mqd_release_lock(d);
	return err;
}

int mq_send(mqd_t des, const char *msg_ptr, size_t msg_size, unsigned msg_prio)
{
	struct mqd *d;
	struct mqueue *q;
	struct message *prev;
	struct message *m;
	int res = 0;

	if (msg_prio >= MQ_PRIO_MAX || msg_prio < 0) {
		errno = EINVAL;
		goto bad;
	}

	d = get_and_lock_mqd(des);

	if (d == NULL) {
		errno = EBADF;
		return -1;
	}

	/* discard volatile qualifier, as the queue will not be modified by
	 * other threads
	 */
	q = (void*)d->queue;

	if (msg_size > d->attr.mq_msgsize) {
		errno = EMSGSIZE;
		goto bad;
	}

	/* Check the queue's permisions */
	if (!(d->flags & (O_RDWR | O_WRONLY))) {
		errno = EPERM;
		goto bad;
	}

	/* If q->curmsg > MQ_MAX_MSG, then the queue is invalid */
	if (q->curmsg > d->attr.mq_maxmsg) {
		errno = EBADMSG;
		goto bad;
	}

	if (q->curmsg == d->attr.mq_maxmsg) {
		if (d->flags & O_NONBLOCK) {
			errno = EAGAIN;
			goto bad;
		} else do {
			mqd_release_lock(d);
			mq_cond_wait(&d->not_full);
			d = get_mqd(des);
			mqd_get_lock(d);
		} while (!(d->flags & O_NONBLOCK));

		q = (void*)d->queue;

		/* O_NONBLOCK was set. */
		if (q->curmsg == d->attr.mq_maxmsg) {

		}

		if (q->curmsg > d->attr.mq_maxmsg) {
			errno = EBADMSG;
			goto bad;
		}
	}

	/* create the message */
	m = mqueue_get_msg(q, q->free_head);

	m->size = msg_size;
	m->flags = MQ_MSG_ALIVE;
	memcpy(m->buffer, msg_ptr, msg_size);

	prev = mqueue_get_msg(q, q->prio_tail[msg_prio]);
	/* Put the new message in the queue. Ignore value of m->prev */
	m->prev = q->prio_tail[msg_prio];
	q->prio_tail[msg_prio] = prev->next = q->free_head;

	/* check if the queue can hold more messages */
	if (m->next == -1) {
		q->free_head = q->free_tail = -1;
		mq_cond_unset(&d->not_full);
	} else {
		q->free_head = m->next;
	}

	m->next = -1;
	d->attr.mq_curmsg = ++q->curmsg;

	/* signal that the queue is not empty */
	if (q->curmsg == 1) {
		mq_cond_set(&d->not_empty);
		mq_cond_set(&d->not_empty_prio[msg_prio]);
	}

	if (0) {
bad:
		res = -1;
	}
	mqd_release_lock(d);
	return res;
}

int mq_unlink(const char *name)
{
	errno = ENOSYS;
	return -1;
}

static int mqdtable_init(void)
{
	if (mqdtab == NULL)
		mqdtab = calloc(1, sizeof(struct mqdtable));

	if (mqdtab != NULL) {
		/* create a lock */
		int i = 1;
		InitializeCriticalSection(&mqdtab->lock);
		EnterCriticalSection(&mqdtab->lock);
		for (; i < MQ_OPEN_MAX; ++i) {
			mqdtab->desc[i - 1].next = &mqdtab->desc[i];
			mqdtab->desc[i].prev = &mqdtab->desc[i - 1];
		}
		mqdtab->free_mqd.head = mqdtab->desc;
		LeaveCriticalSection(&mqdtab->lock);
		return 0;
	}
	return -1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD fdwReason, LPVOID reserved)
{
	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		if (mqdtable_init())
			return FALSE;
		break;
	}
	return TRUE;
}
