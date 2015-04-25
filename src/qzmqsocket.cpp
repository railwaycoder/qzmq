/*
 * Copyright (C) 2012 Justin Karneges
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "qzmqsocket.h"

#include <stdio.h>
#include <assert.h>
#include <QStringList>
#include <QTimer>
#include <QSocketNotifier>
#include <QMutex>
#include <zmq.h>
#include "qzmqcontext.h"

namespace QZmq {

static int get_fd(void *sock)
{
    long long fd;
	size_t opt_len = sizeof(fd);
	int ret = zmq_getsockopt(sock, ZMQ_FD, &fd, &opt_len);
	assert(ret == 0);
    return (int)fd;
}

static void set_subscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_setsockopt(sock, ZMQ_SUBSCRIBE, data, opt_len);
	assert(ret == 0);
}

static void set_unsubscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	zmq_setsockopt(sock, ZMQ_UNSUBSCRIBE, data, opt_len);
	// note: we ignore errors, such as unsubscribing a nonexisting filter
}

static void set_linger(void *sock, int value)
{
	size_t opt_len = sizeof(value);
	int ret = zmq_setsockopt(sock, ZMQ_LINGER, &value, opt_len);
	assert(ret == 0);
}

static int get_identity(void *sock, char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_getsockopt(sock, ZMQ_IDENTITY, data, &opt_len);
	assert(ret == 0);
	return (int)opt_len;
}

static void set_identity(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_setsockopt(sock, ZMQ_IDENTITY, data, opt_len);
	if(ret != 0)
		printf("%d\n", errno);
	assert(ret == 0);
}

#if (ZMQ_VERSION_MAJOR >= 4) || ((ZMQ_VERSION_MAJOR >= 3) && (ZMQ_VERSION_MINOR >= 2))

#define USE_MSG_IO

static bool get_rcvmore(void *sock)
{
	int more;
	size_t opt_len = sizeof(more);
	int ret = zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &opt_len);
	assert(ret == 0);
	return more ? true : false;
}

static int get_events(void *sock)
{
	int events;
	size_t opt_len = sizeof(events);
	int ret = zmq_getsockopt(sock, ZMQ_EVENTS, &events, &opt_len);
	assert(ret == 0);
	return (int)events;
}

static int get_sndhwm(void *sock)
{
	int hwm;
	size_t opt_len = sizeof(hwm);
	int ret = zmq_getsockopt(sock, ZMQ_SNDHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_sndhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_SNDHWM, &v, opt_len);
	assert(ret == 0);
}

static int get_rcvhwm(void *sock)
{
	int hwm;
	size_t opt_len = sizeof(hwm);
	int ret = zmq_getsockopt(sock, ZMQ_RCVHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_rcvhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_RCVHWM, &v, opt_len);
	assert(ret == 0);
}

static int get_hwm(void *sock)
{
	return get_sndhwm(sock);
}

static void set_hwm(void *sock, int value)
{
	set_sndhwm(sock, value);
	set_rcvhwm(sock, value);
}

#else

static bool get_rcvmore(void *sock)
{
	qint64 more;
	size_t opt_len = sizeof(more);
	int ret = zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &opt_len);
	assert(ret == 0);
	return more ? true : false;
}

static int get_events(void *sock)
{
	quint32 events;
	size_t opt_len = sizeof(events);
	int ret = zmq_getsockopt(sock, ZMQ_EVENTS, &events, &opt_len);
	assert(ret == 0);
	return (int)events;
}

static int get_hwm(void *sock)
{
	quint64 hwm;
	size_t opt_len = sizeof(hwm);
	int ret = zmq_getsockopt(sock, ZMQ_HWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_hwm(void *sock, int value)
{
	quint64 v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_HWM, &v, opt_len);
	assert(ret == 0);
}

static int get_sndhwm(void *sock)
{
	return get_hwm(sock);
}

static void set_sndhwm(void *sock, int value)
{
	set_hwm(sock, value);
}

static int get_rcvhwm(void *sock)
{
	return get_hwm(sock);
}

static void set_rcvhwm(void *sock, int value)
{
	set_hwm(sock, value);
}

#endif

Q_GLOBAL_STATIC(QMutex, g_mutex)

class Global
{
public:
	Context context;
	int refs;

	Global() :
		refs(0)
	{
	}
};

static Global *global = 0;

static Context *addGlobalContextRef()
{
	QMutexLocker locker(g_mutex());

	if(!global)
		global = new Global;

	++(global->refs);
	return &(global->context);
}

static void removeGlobalContextRef()
{
	QMutexLocker locker(g_mutex());

	assert(global);
	assert(global->refs > 0);

	--(global->refs);
	if(global->refs == 0)
	{
		delete global;
		global = 0;
	}
}

class Socket::Private : public QObject
{
	Q_OBJECT

public:
	Socket *q;
	bool usingGlobalContext;
	Context *context;
	void *sock;
	QSocketNotifier *sn_read;
	bool canWrite, canRead;
	QList<QByteArray> pendingRead;
	bool readComplete;
	QList< QList<QByteArray> > pendingWrites;
	QTimer *updateTimer;
	bool pendingUpdate;
	int shutdownWaitTime;
	bool writeQueueEnabled;

	Private(Socket *_q, Socket::Type type, Context *_context) :
		QObject(_q),
		q(_q),
		canWrite(false),
		canRead(false),
		readComplete(false),
		pendingUpdate(false),
		shutdownWaitTime(-1),
		writeQueueEnabled(true)
	{
		if(_context)
		{
			usingGlobalContext = false;
			context = _context;
		}
		else
		{
			usingGlobalContext = true;
			context = addGlobalContextRef();
		}

		int ztype;
		switch(type)
		{
			case Socket::Pair: ztype = ZMQ_PAIR; break;
			case Socket::Dealer: ztype = ZMQ_DEALER; break;
			case Socket::Router: ztype = ZMQ_ROUTER; break;
			case Socket::Req: ztype = ZMQ_REQ; break;
			case Socket::Rep: ztype = ZMQ_REP; break;
			case Socket::Push: ztype = ZMQ_PUSH; break;
			case Socket::Pull: ztype = ZMQ_PULL; break;
			case Socket::Pub: ztype = ZMQ_PUB; break;
			case Socket::Sub: ztype = ZMQ_SUB; break;
			default:
				assert(0);
		}

		sock = zmq_socket(context->context(), ztype);
		assert(sock != NULL);

		sn_read = new QSocketNotifier(get_fd(sock), QSocketNotifier::Read, this);
		connect(sn_read, SIGNAL(activated(int)), SLOT(sn_read_activated()));
		sn_read->setEnabled(true);

		updateTimer = new QTimer(this);
		connect(updateTimer, SIGNAL(timeout()), SLOT(update_timeout()));
		updateTimer->setSingleShot(true);
	}

	~Private()
	{
		set_linger(sock, shutdownWaitTime);
		zmq_close(sock);

		if(usingGlobalContext)
			removeGlobalContextRef();
	}

	QList<QByteArray> read()
	{
		if(readComplete)
		{
			QList<QByteArray> out = pendingRead;
			pendingRead.clear();
			readComplete = false;

			if(canRead && !pendingUpdate)
			{
				pendingUpdate = true;
				updateTimer->start();
			}

			return out;
		}
		else
			return QList<QByteArray>();
	}

	void write(const QList<QByteArray> &message)
	{
		assert(!message.isEmpty());

		if(writeQueueEnabled)
		{
			pendingWrites += message;

			if(canWrite && !pendingUpdate)
			{
				pendingUpdate = true;
				updateTimer->start();
			}
		}
		else
		{
			for(int n = 0; n < message.count(); ++n)
			{
				const QByteArray &buf = message[n];

				zmq_msg_t msg;
				int ret = zmq_msg_init_size(&msg, buf.size());
				assert(ret == 0);
				memcpy(zmq_msg_data(&msg), buf.data(), buf.size());
#ifdef USE_MSG_IO
				ret = zmq_msg_send(&msg, sock, ZMQ_DONTWAIT | (n + 1 < message.count() ? ZMQ_SNDMORE : 0));
#else
				ret = zmq_send(sock, &msg, ZMQ_NOBLOCK | (n + 1 < message.count() ? ZMQ_SNDMORE : 0));
#endif
				if(ret == -1)
				{
					assert(errno == EAGAIN);
					ret = zmq_msg_close(&msg);
					assert(ret == 0);
					return;
				}

				ret = zmq_msg_close(&msg);
				assert(ret == 0);
			}
		}
	}

	void processEvents(bool *readyRead, int *messagesWritten)
	{
		bool again;
		do
		{
			again = false;

			int flags = get_events(sock);

			if(flags & ZMQ_POLLOUT)
			{
				canWrite = true;
				again = tryWrite(messagesWritten) || again;
			}
			else
				canWrite = false;

			if(flags & ZMQ_POLLIN)
			{
				canRead = true;
				again = tryRead(readyRead) || again;
			}
		} while(again);
	}

	bool tryWrite(int *messagesWritten)
	{
		if(!pendingWrites.isEmpty())
		{
			QList<QByteArray> &message = pendingWrites.first();
			QByteArray buf = message.first();

			// whether this write succeeds or not, we assume we
			//   can't write afterwards
			canWrite = false;

			zmq_msg_t msg;
			int ret = zmq_msg_init_size(&msg, buf.size());
			assert(ret == 0);
			memcpy(zmq_msg_data(&msg), buf.data(), buf.size());
#ifdef USE_MSG_IO
			ret = zmq_msg_send(&msg, sock, ZMQ_DONTWAIT | (message.count() > 1 ? ZMQ_SNDMORE : 0));
#else
			ret = zmq_send(sock, &msg, ZMQ_NOBLOCK | (message.count() > 1 ? ZMQ_SNDMORE : 0));
#endif
			if(ret == -1)
			{
				assert(errno == EAGAIN);
				ret = zmq_msg_close(&msg);
				assert(ret == 0);

				// if send fails, there should not be any
				//   events change
				return false;
			}

			ret = zmq_msg_close(&msg);
			assert(ret == 0);

			message.removeFirst();
			if(message.isEmpty())
			{
				pendingWrites.removeFirst();
				++(*messagesWritten);
			}

			return true;
		}

		return false;
	}

	// return true if a read was performed
	bool tryRead(bool *readyRead)
	{
		if(!readComplete)
		{
			zmq_msg_t msg;
			int ret = zmq_msg_init(&msg);
			assert(ret == 0);
#ifdef USE_MSG_IO
			ret = zmq_msg_recv(&msg, sock, 0);
#else
			ret = zmq_recv(sock, &msg, 0);
#endif
			assert(ret != -1);
			QByteArray buf((const char *)zmq_msg_data(&msg), zmq_msg_size(&msg));
			ret = zmq_msg_close(&msg);
			assert(ret == 0);

			pendingRead += buf;
			canRead = false;

			if(!get_rcvmore(sock))
			{
				readComplete = true;
				*readyRead = true;
			}

			return true;
		}

		return false;
	}

public slots:
	void sn_read_activated()
	{
		bool readyRead = false;
		int messagesWritten = 0;

		processEvents(&readyRead, &messagesWritten);

		if(readyRead)
			emit q->readyRead();

		if(messagesWritten > 0)
			emit q->messagesWritten(messagesWritten);
	}

	void update_timeout()
	{
		pendingUpdate = false;

		bool readyRead = false;
		int messagesWritten = 0;

		if(canWrite)
		{
			if(tryWrite(&messagesWritten))
				processEvents(&readyRead, &messagesWritten);
		}

		if(canRead)
		{
			if(tryRead(&readyRead))
				processEvents(&readyRead, &messagesWritten);
		}

		if(readyRead)
			emit q->readyRead();

		if(messagesWritten > 0)
			emit q->messagesWritten(messagesWritten);
	}
};

Socket::Socket(Type type, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, type, 0);
}

Socket::Socket(Type type, Context *context, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, type, context);
}

Socket::~Socket()
{
	delete d;
}

void Socket::setShutdownWaitTime(int msecs)
{
	d->shutdownWaitTime = msecs;
}

void Socket::setWriteQueueEnabled(bool enable)
{
	d->writeQueueEnabled = enable;
}

void Socket::subscribe(const QByteArray &filter)
{
	set_subscribe(d->sock, filter.data(), filter.size());
}

void Socket::unsubscribe(const QByteArray &filter)
{
	set_unsubscribe(d->sock, filter.data(), filter.size());
}

QByteArray Socket::identity() const
{
	QByteArray buf(255, 0);
	buf.resize(get_identity(d->sock, buf.data(), buf.size()));
	return buf;
}

void Socket::setIdentity(const QByteArray &id)
{
	set_identity(d->sock, id.data(), id.size());
}

int Socket::hwm() const
{
	return get_hwm(d->sock);
}

void Socket::setHwm(int hwm)
{
	set_hwm(d->sock, hwm);
}

int Socket::sendHwm() const
{
	return get_sndhwm(d->sock);
}

int Socket::receiveHwm() const
{
	return get_rcvhwm(d->sock);
}

void Socket::setSendHwm(int hwm)
{
	set_sndhwm(d->sock, hwm);
}

void Socket::setReceiveHwm(int hwm)
{
	set_rcvhwm(d->sock, hwm);
}

void Socket::connectToAddress(const QString &addr)
{
	int ret = zmq_connect(d->sock, addr.toUtf8().data());
	assert(ret == 0);
}

bool Socket::bind(const QString &addr)
{
	int ret = zmq_bind(d->sock, addr.toUtf8().data());
	if(ret != 0)
		return false;

	return true;
}

bool Socket::canRead() const
{
	return d->readComplete;
}

bool Socket::canWriteImmediately() const
{
	return d->canWrite;
}

QList<QByteArray> Socket::read()
{
	return d->read();
}

void Socket::write(const QList<QByteArray> &message)
{
	d->write(message);
}

}

#include "qzmqsocket.moc"
