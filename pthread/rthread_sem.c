/*	$OpenBSD: rthread_sem.c,v 1.26 2017/09/05 02:40:54 guenther Exp $ */
/*
 * Copyright (c) 2004,2005,2013 Ted Unangst <tedu@openbsd.org>
 * All Rights Reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include "rthread.h"

#define SHARED_IDENT ((void *)-1)

/* SHA256_DIGEST_STRING_LENGTH includes nul */
/* "/tmp/" + sha256 + ".sem" */
#define SEM_PATH_SIZE (5 + SHA256_DIGEST_STRING_LENGTH + 4)

/* long enough to be hard to guess */
#define SEM_RANDOM_NAME_LEN	10

/*
 * Size of memory to be mmap()'ed by named semaphores.
 * Should be >= SEM_PATH_SIZE and page-aligned.
 */
#define SEM_MMAP_SIZE	_thread_pagesize

#define SEM_VALUE_MAX	     UINT_MAX

/*
 * Internal implementation of semaphores
 */
int
_sem_wait(sem_t sem, int tryonly, const struct timespec *abstime,
    int *delayed_cancel)
{
	void *ident = (void *)&sem->waitcount;
	// This should never happen
	int r;

	if (sem->shared)
		ident = SHARED_IDENT;

	phal_mutex_lock(&sem->lock);
	if (sem->value) {
		sem->value--;
		r = 0;
	} else if (tryonly) {
		r = EAGAIN;
	} else {
		sem->waitcount++;
		do {
			int nsec = 0;
			if (abstime) {
				nsec += abstime->tv_sec * 1000 * 1000 * 1000;
				nsec += abstime->tv_nsec;
			}
			r = phal_semaphore_wait(ident, &sem->lock, nsec);
			// TODO: Delayed_cancel ?
			// At this point, I already have lock ! But do I need it ?
			//_spinlock(&sem->lock);
			/* ignore interruptions other than cancelation */
			if (r == EINTR && (delayed_cancel == NULL ||
			    *delayed_cancel == 0))
				r = 0;
		} while (r == 0 && sem->value == 0);
		// TODO: Done by phal_semaphore_wait ?
		//sem->waitcount--;
		if (r == 0)
			sem->value--;
	}
	phal_mutex_unlock(&sem->lock);
	return (r);
}

/* always increment count */
int
_sem_post(sem_t sem)
{
	void *ident = (void *)&sem->waitcount;
	int rv = 0;

	if (sem->shared)
		ident = SHARED_IDENT;

	phal_mutex_lock(&sem->lock);
	sem->value++;
	if (sem->waitcount) {
		phal_semaphore_signal(ident);
		rv = 1;
	}
	phal_mutex_unlock(&sem->lock);
	return (rv);
}

/*
 * exported semaphores
 */
int
sem_init(sem_t *semp, int pshared, unsigned int value)
{
	sem_t sem;

	if (value > SEM_VALUE_MAX) {
		errno = EINVAL;
		return (-1);
	}

	if (pshared) {
		errno = EPERM;
		return (-1);
#ifdef notyet
		char name[SEM_RANDOM_NAME_LEN];
		sem_t *sempshared;
		int i;

		for (;;) {
			for (i = 0; i < SEM_RANDOM_NAME_LEN - 1; i++)
				name[i] = arc4random_uniform(255) + 1;
			name[SEM_RANDOM_NAME_LEN - 1] = '\0';
			sempshared = sem_open(name, O_CREAT | O_EXCL, 0, value);
			if (sempshared != SEM_FAILED)
				break;
			if (errno == EEXIST)
				continue;
			if (errno != EPERM)
				errno = ENOSPC;
			return (-1);
		}

		/* unnamed semaphore should not be opened twice */
		if (sem_unlink(name) == -1) {
			sem_close(sempshared);
			errno = ENOSPC;
			return (-1);
		}

		*semp = *sempshared;
		free(sempshared);
		return (0);
#endif
	}

	sem = calloc(1, sizeof(*sem));
	if (!sem) {
		errno = ENOSPC;
		return (-1);
	}
	phal_mutex_create(&sem->lock);
	sem->value = value;
	*semp = sem;

	return (0);
}

int
sem_destroy(sem_t *semp)
{
	sem_t sem;

	if (!_threads_ready)		 /* for SEM_MMAP_SIZE */
		_rthread_init();

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	if (sem->waitcount) {
#define MSG "sem_destroy on semaphore with waiters!\n"
		write(2, MSG, sizeof(MSG) - 1);
#undef MSG
		errno = EBUSY;
		return (-1);
	}

	*semp = NULL;
	if (sem->shared)
		return ENOSYS;
		// TODO: munmap
		//munmap(sem, SEM_MMAP_SIZE);
	else
		free(sem);

	return (0);
}

int
sem_getvalue(sem_t *semp, int *sval)
{
	sem_t sem;

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	phal_mutex_lock(&sem->lock);
	*sval = sem->value;
	phal_mutex_unlock(&sem->lock);

	return (0);
}

int
sem_post(sem_t *semp)
{
	sem_t sem;

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	_sem_post(sem);

	return (0);
}

int
sem_wait(sem_t *semp)
{
	pthread_t self = pthread_self();
	sem_t sem;
	int r;
	//PREP_CANCEL_POINT(tib);

	if (!_threads_ready)
		_rthread_init();

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	//ENTER_DELAYED_CANCEL_POINT(tib, self);
	r = _sem_wait(sem, 0, NULL, &self->delayed_cancel);
	//LEAVE_CANCEL_POINT_INNER(tib, r);

	if (r) {
		errno = r;
		return (-1);
	}

	return (0);
}

int
sem_timedwait(sem_t *semp, const struct timespec *abstime)
{
	return ENOSYS;
#if 0
	struct tib *tib = TIB_GET();
	pthread_t self;
	sem_t sem;
	int r;
	PREP_CANCEL_POINT(tib);

	if (!_threads_ready)
		_rthread_init();
	self = tib->tib_thread;

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	ENTER_DELAYED_CANCEL_POINT(tib, self);
	r = _sem_wait(sem, 0, abstime, &self->delayed_cancel);
	LEAVE_CANCEL_POINT_INNER(tib, r);

	if (r) {
		errno = r == EWOULDBLOCK ? ETIMEDOUT : r;
		return (-1);
	}

	return (0);
#endif
}

int
sem_trywait(sem_t *semp)
{
	sem_t sem;
	int r;

	if (!semp || !(sem = *semp)) {
		errno = EINVAL;
		return (-1);
	}

	r = _sem_wait(sem, 1, NULL, NULL);

	if (r) {
		errno = r;
		return (-1);
	}

	return (0);
}


/*static void
makesempath(const char *origpath, char *sempath, size_t len)
{
	char buf[SHA256_DIGEST_STRING_LENGTH];

	SHA256Data(origpath, strlen(origpath), buf);
	snprintf(sempath, len, "/tmp/%s.sem", buf);
}*/

sem_t *
sem_open(const char *name, int oflag, ...)
{
	errno = ENOSYS;
	return SEM_FAILED;
#if 0
	char sempath[SEM_PATH_SIZE];
	struct stat sb;
	sem_t sem, *semp;
	unsigned int value = 0;
	int created = 0, fd;

	if (!_threads_ready)
		_rthread_init();

	if (oflag & ~(O_CREAT | O_EXCL)) {
		errno = EINVAL;
		return (SEM_FAILED);
	}

	if (oflag & O_CREAT) {
		va_list ap;
		va_start(ap, oflag);
		/* 3rd parameter mode is not used */
		va_arg(ap, mode_t);
		value = va_arg(ap, unsigned);
		va_end(ap);

		if (value > SEM_VALUE_MAX) {
			errno = EINVAL;
			return (SEM_FAILED);
		}
	}

	makesempath(name, sempath, sizeof(sempath));
	fd = open(sempath, O_RDWR | O_NOFOLLOW | oflag, 0600);
	if (fd == -1)
		return (SEM_FAILED);
	if (fstat(fd, &sb) == -1 || !S_ISREG(sb.st_mode)) {
		close(fd);
		errno = EINVAL;
		return (SEM_FAILED);
	}
	if (sb.st_uid != geteuid()) {
		close(fd);
		errno = EPERM;
		return (SEM_FAILED);
	}
	if (sb.st_size != (off_t)SEM_MMAP_SIZE) {
		if (!(oflag & O_CREAT)) {
			close(fd);
			errno = EINVAL;
			return (SEM_FAILED);
		}
		if (sb.st_size != 0) {
			close(fd);
			errno = EINVAL;
			return (SEM_FAILED);
		}
		if (ftruncate(fd, SEM_MMAP_SIZE) == -1) {
			close(fd);
			errno = EINVAL;
			return (SEM_FAILED);
		}

		created = 1;
	}
	sem = mmap(NULL, SEM_MMAP_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	close(fd);
	if (sem == MAP_FAILED) {
		errno = EINVAL;
		return (SEM_FAILED);
	}
	semp = malloc(sizeof(*semp));
	if (!semp) {
		munmap(sem, SEM_MMAP_SIZE);
		errno = ENOSPC;
		return (SEM_FAILED);
	}
	if (created) {
		phal_mutex_create(&sem->lock);
		sem->value = value;
		sem->shared = 1;
	}
	*semp = sem;

	return (semp);
#endif
}

int
sem_close(sem_t *semp)
{
	sem_t sem;

	if (!semp || !(sem = *semp) || !sem->shared) {
		errno = EINVAL;
		return (-1);
	}

	return ENOSYS;
	//*semp = NULL;
	//munmap(sem, SEM_MMAP_SIZE);
	//free(semp);

	//return (0);
}

int
sem_unlink(const char *name)
{
	//char sempath[SEM_PATH_SIZE];

	return ENOSYS;
	//makesempath(name, sempath, sizeof(sempath));
	//return (unlink(sempath));
}
