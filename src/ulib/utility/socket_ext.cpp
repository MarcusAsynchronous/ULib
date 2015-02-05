// ============================================================================
//
// = LIBRARY
//    ULib - c++ library
//
// = FILENAME
//    socket_ext.cpp
//
// = AUTHOR
//    Stefano Casazza
//
// ============================================================================

#include <ulib/file.h>
#include <ulib/notifier.h>
#include <ulib/container/vector.h>
#include <ulib/utility/interrupt.h>
#include <ulib/net/server/client_image.h>

#ifdef _MSWINDOWS_
#  include <ws2tcpip.h>
#elif defined(HAVE_NETPACKET_PACKET_H) && !defined(U_ALL_CPP)
#  include <net/if.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#  include <sys/ioctl.h>
#endif

#ifdef USE_C_ARES
#  include <ares.h>
#endif

vPFi  USocketExt::byte_read_hook; // it allows the generation of a progress meter during upload...
vPFsu USocketExt::read_buffer_resize = UString::_reserve;

/**
 * Socket I/O - read while not received almost count data
 *
 * @timeoutMS  specified the timeout value, in milliseconds.
 *             A negative value indicates no timeout, i.e. an infinite wait
 * @time_limit specified the maximum execution time, in seconds. If set to zero, no time limit is imposed
 */

bool USocketExt::read(USocket* sk, UString& buffer, uint32_t count, int timeoutMS, uint32_t time_limit)
{
   U_TRACE(0, "USocketExt::read(%p,%.*S,%u,%d,%u,%p)", sk, U_STRING_TO_TRACE(buffer), count, timeoutMS, time_limit)

   U_INTERNAL_ASSERT_POINTER(sk)
   U_INTERNAL_ASSERT(sk->isConnected())

   char* ptr;
   long timeout = 0;
   int byte_read = 0;
   ssize_t value; // = -1;
   uint32_t start  = buffer.size(), // NB: read buffer can have previous data...
            ncount = buffer.space(),
            chunk  = count;
   bool blocking = sk->isBlocking();

   if (LIKELY(chunk < U_CAPACITY)) chunk = U_CAPACITY;

   if (UNLIKELY(ncount < chunk))
      {
      read_buffer_resize(buffer, chunk); 

      ncount = buffer.space();
      }

   ptr = buffer.c_pointer(start);

read:
   if (blocking       &&
       timeoutMS != 0 &&
       UNotifier::waitForRead(sk->iSockDesc, timeoutMS) != 1)
      {
      goto error;
      }

   value = sk->recv(ptr + byte_read, ncount);

   if (value <= 0)
      {
      if (value == -1)
         {
error:   U_INTERNAL_DUMP("errno = %d", errno)

         if (errno != EAGAIN)
            {
            sk->iState = (errno == ECONNRESET ? USocket::EPOLLERROR : USocket::BROKEN);
close:
            sk->closesocket();

            sk->iState = USocket::CLOSE;
            }
         else if (timeoutMS != 0)
            {
            if (UNotifier::waitForRead(sk->iSockDesc, timeoutMS) == 1) goto read;

            sk->iState |= USocket::TIMEOUT;
            }

         U_INTERNAL_DUMP("sk->state = %d %B", sk->iState, sk->iState)
         }
      else
         {
         U_INTERNAL_ASSERT_EQUALS(value, 0)

         if (byte_read == 0 ||
             sk->shutdown(SHUT_RD) == false)
            {
            if (U_ClientImage_parallelization == 1) U_RETURN(false); // 1 => child of parallelization

            goto close;
            }

         UClientImage_Base::setCloseConnection();
         }

      goto done;
      }

   byte_read += value;

   U_INTERNAL_DUMP("byte_read = %d", byte_read)

   U_INTERNAL_ASSERT_MAJOR(byte_read, 0)

   if (byte_read < (int)count)
      {
      U_INTERNAL_ASSERT_DIFFERS(count, U_SINGLE_READ)

      if (time_limit &&
          sk->checkTime(time_limit, timeout) == false) // NB: may be is attacked by a "slow loris"... http://lwn.net/Articles/337853/
         {
         sk->iState |= USocket::TIMEOUT;

         goto done;
         }

      if (byte_read_hook) byte_read_hook(byte_read);

      ncount -= value;

      goto read;
      }

   if (value == (ssize_t)ncount)
      {
#  ifdef DEBUG
   // U_MESSAGE("USocketExt::read(%u) ran out of buffer space(%u)", count, ncount);
#  endif

      buffer.size_adjust_force(start + byte_read); // NB: we force because string can be referenced...

      // NB: may be there are available more bytes to read...

      read_buffer_resize(buffer, ncount * 2);

      ptr = buffer.c_pointer(start);

#  ifdef USE_LIBSSL
      if (sk->isSSL(true))
         {
         /** 
          * When packets in SSL arrive at a destination, they are pulled off the socket in chunks of sizes
          * controlled by the encryption protocol being used, decrypted, and placed in SSL-internal buffers.
          * The buffer content is then transferred to the application program through SSL_read(). If you've
          * read only part of the decrypted data, there will still be pending input data on the SSL connection,
          * but it won't show up on theunderlying file descriptor via select(). Your code needs to call
          * SSL_pending() explicitly to see if there is any pending data to be read
          */

         uint32_t available = ((USSLSocket*)sk)->pending();

         if (available)
            {
            byte_read += sk->recv(ptr + byte_read, available);

            goto done;
            }
         }
#  endif

      ncount    = buffer.space();
      timeoutMS = 0;

      goto read;
      }

#ifdef USE_LIBSSL
   if (sk->isSSL(true) == false)
#endif
   {
#if !defined(U_SERVER_CAPTIVE_PORTAL) && !defined(_MSWINDOWS_) && defined(HAVE_EPOLL_WAIT)
   if ((UNotifier::add_mask & EPOLLET) != 0)
      {
      U_INTERNAL_ASSERT_DIFFERS(USocket::server_flags & O_NONBLOCK, 0)

      buffer.size_adjust_force(start + byte_read); // NB: we force because string can be referenced...

      ncount    = buffer.space();
      timeoutMS = 0;

      goto read;
      }
#endif
   }

done:
   U_INTERNAL_DUMP("byte_read = %d", byte_read)

   if (byte_read)
      {
      start += byte_read;

      if (start > buffer.size()) buffer.size_adjust_force(start); // NB: we force because string can be referenced...

      if (byte_read >= (int)count &&
          sk->iState != USocket::CLOSE)
         {
         U_RETURN(true);
         }
      }

   U_RETURN(false);
}

/**
 * Socket I/O - read while not received token, return position of token in buffer read
 *
 * @param timeoutMS specified the timeout value, in milliseconds.
 *        A negative value indicates no timeout, i.e. an infinite wait
 */

uint32_t USocketExt::readWhileNotToken(USocket* sk, UString& buffer, const char* token, uint32_t token_len, int timeoutMS)
{
   U_TRACE(0, "USocketExt::readWhileNotToken(%p,%.*S,%.*S,%u,%d)", sk, U_STRING_TO_TRACE(buffer), token_len, token, token_len, timeoutMS)

   uint32_t start = buffer.size();

   while (USocketExt::read(sk, buffer, U_SINGLE_READ, timeoutMS))
      {
      uint32_t pos_token = buffer.find(token, start, token_len);

      if (pos_token != U_NOT_FOUND) U_RETURN(pos_token);

      U_ASSERT_MAJOR(buffer.size(), token_len)

      start = buffer.size() - token_len;
      }

   U_RETURN(U_NOT_FOUND);
}

// write data

int USocketExt::write(USocket* sk, const char* ptr, uint32_t count, int timeoutMS)
{
   U_TRACE(0, "USocketExt::write(%p,%.*S,%u,%d)", sk, count, ptr, count, timeoutMS)

   U_INTERNAL_ASSERT_POINTER(sk)
   U_INTERNAL_ASSERT_MAJOR(count, 0)
   U_INTERNAL_ASSERT(sk->isConnected())

   ssize_t value;
   int byte_written = 0;
   bool blocking = sk->isBlocking();

write:
   if (blocking       &&
       timeoutMS != 0 &&
       UNotifier::waitForWrite(sk->iSockDesc, timeoutMS) != 1)
      {
      goto error;
      }

   value = sk->send(ptr + byte_written, count);

   if (value <= 0)
      {
      if (value == -1)
         {
error:   U_INTERNAL_DUMP("errno = %d", errno)

         if (errno != EAGAIN)
            {
            sk->iState = USocket::BROKEN;

            sk->closesocket();

            sk->iState = USocket::CLOSE;
            }
         else if (timeoutMS != 0)
            {
            if (UNotifier::waitForWrite(sk->iSockDesc, timeoutMS) == 1) goto write;

            sk->iState |= USocket::TIMEOUT;
            }

         U_INTERNAL_DUMP("sk->state = %d %B", sk->iState, sk->iState)
         }

      U_RETURN(byte_written);
      }

   byte_written += value;

   U_INTERNAL_DUMP("byte_written = %d", byte_written)

   U_INTERNAL_ASSERT_MAJOR(byte_written, 0)

   if (byte_written < (int)count)
      {
      count    -= value;
      timeoutMS = 0;

      goto write;
      }

   U_RETURN(byte_written);
}

// write data from multiple buffers

int USocketExt::writev(USocket* sk, struct iovec* iov, int iovcnt, uint32_t count, int timeoutMS)
{
   U_TRACE(0, "USocketExt::writev(%p,%p,%d,%u,%d)", sk, iov, iovcnt, count, timeoutMS)

   U_INTERNAL_ASSERT_POINTER(sk)
   U_INTERNAL_ASSERT_MAJOR(count, 0)
   U_INTERNAL_ASSERT(sk->isConnected())

   if (iovcnt == 1) return write(sk, (const char*)iov[0].iov_base, iov[0].iov_len, timeoutMS);

   U_INTERNAL_ASSERT_MAJOR(iovcnt, 1)

   bool blocking;
   ssize_t value;
   int i, idx, byte_written = 0;

#if defined(USE_LIBSSL) || defined(_MSWINDOWS_)
#if defined(USE_LIBSSL)
   if (sk->isSSL(true))
      {
      int sz;

      if (count <= U_CAPACITY)
         {
         UString buffer(U_CAPACITY);

         for (i = 0; i < iovcnt; ++i)
            {
            if ((sz = iov[i].iov_len)) (void) buffer.append((const char*)iov[i].iov_base, sz);
            }

         return write(sk, buffer, timeoutMS);
         }
#endif
      for (i = 0; i < iovcnt; ++i)
         {
         if ((sz = iov[i].iov_len))
            {
            value = write(sk, (const char*)iov[i].iov_base, sz, timeoutMS);

            if (value <= 0) break;

            byte_written += value;

            if (value != sz) break;
            }
         }

      U_RETURN(byte_written);
#  ifdef USE_LIBSSL
      }
#  endif
#endif

   idx      = 0;
   blocking = sk->isBlocking();

write:
   if (blocking       &&
       timeoutMS != 0 &&
       UNotifier::waitForWrite(sk->iSockDesc, timeoutMS) != 1)
      {
      goto error;
      }

   value = U_SYSCALL(writev, "%d,%p,%d", sk->iSockDesc, iov, iovcnt);

   if (value <= 0)
      {
      if (value == -1)
         {
error:   U_INTERNAL_DUMP("errno = %d", errno)

         if (errno != EAGAIN)
            {
            if (errno == EINTR &&
                UInterrupt::checkForEventSignalPending())
               {
               goto write;
               }

            sk->iState = USocket::BROKEN;

            sk->closesocket();

            sk->iState = USocket::CLOSE;
            }
         else if (timeoutMS != 0)
            {
            if (UNotifier::waitForWrite(sk->iSockDesc, timeoutMS) == 1) goto write;

            sk->iState |= USocket::TIMEOUT;
            }

         U_INTERNAL_DUMP("sk->state = %d %B", sk->iState, sk->iState)
         }

      U_RETURN(byte_written);
      }

   byte_written += value;

   U_INTERNAL_DUMP("byte_written = %d", byte_written)

   U_INTERNAL_ASSERT_MAJOR(byte_written, 0)

   if ((uint32_t)value < count)
      {
      count -= value;

      while ((size_t)value >= iov[idx].iov_len)
         {
         value -= iov[idx].iov_len;
                  iov[idx].iov_len = 0;

         ++idx;

         U_INTERNAL_ASSERT_MINOR(idx, iovcnt)
         }

      U_INTERNAL_DUMP("iov[%d].iov_len = %d", idx, iov[idx].iov_len)

      U_INTERNAL_ASSERT_MAJOR(iov[idx].iov_len, (size_t)value)

      iov[idx].iov_len -= value;
      iov[idx].iov_base = value + (char*)iov[idx].iov_base;

      iov    += idx;
      iovcnt -= idx;

      timeoutMS = 0;

      goto write;
      }

   U_RETURN(byte_written);
}

int USocketExt::writev(USocket* sk, struct iovec* iov, int iovcnt, uint32_t count, int timeoutMS, uint32_t cloop)
{
   U_TRACE(0, "USocketExt::writev(%p,%p,%d,%u,%d,%u)", sk, iov, iovcnt, count, timeoutMS, cloop)

   U_INTERNAL_ASSERT_POINTER(sk)
   U_INTERNAL_ASSERT_MAJOR(count, 0)
   U_INTERNAL_ASSERT_MAJOR(cloop, 0)
   U_INTERNAL_ASSERT_MINOR(iovcnt, 256)
   U_INTERNAL_ASSERT(sk->isConnected())

   struct iovec _iov[256];

   char* ptr   = (char*)_iov;
   uint32_t sz = sizeof(struct iovec) * iovcnt;

   u__memcpy(ptr, iov, sz, __PRETTY_FUNCTION__);

#ifdef U_PIPELINE_HOMOGENEOUS_DISABLE
   U_INTERNAL_ASSERT_EQUALS(cloop, 1)
#else
   if (cloop > 1)
      {
      for (uint32_t i = 1; i < cloop; ++i)
         {
                   ptr += sz;
         u__memcpy(ptr, iov, sz, __PRETTY_FUNCTION__);
         }

      iov     = _iov;
      iovcnt *= cloop;
      }
#endif

   U_INTERNAL_DUMP("iov[0].iov_len = %d iov[1].iov_len = %d", iov[0].iov_len, iov[1].iov_len)

   int byte_written = writev(sk, iov, iovcnt, count, timeoutMS);

   if (cloop == 1) u__memcpy(iov, _iov, sz, __PRETTY_FUNCTION__);

   U_INTERNAL_DUMP("iov[0].iov_len = %d iov[1].iov_len = %d", iov[0].iov_len, iov[1].iov_len)

   U_RETURN(byte_written);
}

void USocketExt::setRemoteInfo(USocket* sk, UString& logbuf)
{
   U_TRACE(0, "USocketExt::setRemoteInfo(%p,%.*S)", sk, U_STRING_TO_TRACE(logbuf))

   UString x(100U);

   x.snprintf("%2d '%s:%u'", sk->iSockDesc, sk->cRemoteAddress.pcStrAddress, sk->iRemotePort);

   (void) logbuf.insert(0, x);
}

// Send a command to a server and wait for a response (single line)

int USocketExt::vsyncCommand(USocket* sk, char* buffer, uint32_t buffer_size, const char* format, va_list argp)
{
   U_TRACE(0, "USocketExt::vsyncCommand(%p,%p,%u,%S)", sk, buffer, buffer_size, format)

   U_INTERNAL_ASSERT(sk->isOpen())

   uint32_t buffer_len = u__vsnprintf(buffer, buffer_size-2, format, argp);

   buffer[buffer_len++] = '\r';
   buffer[buffer_len++] = '\n';

   int n        =  sk->send(buffer, buffer_len),
       response = (sk->checkIO(n) ? readLineReply(sk, buffer, buffer_size) : (int)USocket::BROKEN);

   U_RETURN(response);
}

// Send a command to a server and wait for a response (multi line)

int USocketExt::vsyncCommandML(USocket* sk, char* buffer, uint32_t buffer_size, const char* format, va_list argp)
{
   U_TRACE(0, "USocketExt::vsyncCommandML(%p,%p,%u,%S)", sk, buffer, buffer_size, format)

   U_INTERNAL_ASSERT(sk->isOpen())

   uint32_t buffer_len = u__vsnprintf(buffer, buffer_size-2, format, argp);

   buffer[buffer_len++] = '\r';
   buffer[buffer_len++] = '\n';

   int n        =  sk->send(buffer, buffer_len),
       response = (sk->checkIO(n) ? readMultilineReply(sk, buffer, buffer_size) : (int)USocket::BROKEN);

   U_RETURN(response);
}

// Send a command to a server and wait for a response (check for token line)

int USocketExt::vsyncCommandToken(USocket* sk, UString& buffer, const char* format, va_list argp)
{
   U_TRACE(1, "USocketExt::vsyncCommandToken(%p,%.*S,%S)", sk, U_STRING_TO_TRACE(buffer), format)

   U_INTERNAL_ASSERT(sk->isOpen())
   U_INTERNAL_ASSERT_EQUALS((bool)buffer, false)

   static uint32_t cmd_count;

   char token[32];
   uint32_t token_len = u__snprintf(token, sizeof(token), "U%04u ", cmd_count++);

   U_INTERNAL_DUMP("token = %.*S", token_len, token)

   char* p = buffer.data();

   U_MEMCPY(p, token, token_len);

   uint32_t buffer_len = token_len + u__vsnprintf(p+token_len, buffer.capacity(), format, argp);

   p[buffer_len++] = '\r';
   p[buffer_len++] = '\n';

   int n = sk->send(p, buffer_len);

   if (sk->checkIO(n))
      {
      uint32_t pos_token = USocketExt::readWhileNotToken(sk, buffer, token, token_len);

      if (pos_token != U_NOT_FOUND)
         {
                          U_ASSERT(buffer.c_char(buffer.size()-1) == '\n')
#     ifdef DEBUG
         if (pos_token) { U_ASSERT(buffer.c_char(pos_token-1)     == '\n') }
#     endif

         U_RETURN(pos_token + token_len);
         }

      U_RETURN(U_NOT_FOUND);
      }

   U_RETURN(USocket::BROKEN);
}

U_NO_EXPORT inline bool USocketExt::parseCommandResponse(char* buffer, int r, int response)
{
   U_TRACE(0, "USocketExt::parseCommandResponse(%p,%d,%d)", buffer, r, response)

   /**
    * Thus the format for multi-line replies is that the first line will begin with the exact required reply code,
    * followed immediately by a Hyphen, "-" (also known as Minus), followed by text. The last line will begin with
    * the same code, followed immediately by Space <SP>, optionally some text, and the Telnet end-of-line code.
    * For example:
    * 123-First line
    *    Second line
    *    234 A line beginning with numbers
    *    123 The last line
    * The user-process then simply needs to search for the second occurrence of the same reply code, followed by
    * <SP> (Space), at the beginning of a line, and ignore all intermediary lines. If an intermediary line begins
    * with a 3-digit number, the Server must pad the front to avoid confusion.
    */

   int complete = 2;

   if (buffer[3] == '-')
      {
      complete = 0;

      for (int i = 0; i < r; ++i)
         {
         if (buffer[i] == '\n')
            {
            if (complete == 1)
               {
               complete = 2;

               break;
               }

            U_INTERNAL_DUMP("buffer = %S", buffer+i+1)

            if (buffer[i+4] == ' ')
               {
               int j = -1;

               (void) sscanf(buffer+i+1, "%3i", &j);

               U_INTERNAL_DUMP("j = %d response = %d", j, response)

               if (j == response) complete = 1;
               }
            }
         }
      }

   U_INTERNAL_DUMP("complete = %d", complete)

   U_RETURN(complete != 2);
}

int USocketExt::readLineReply(USocket* sk, char* buffer, uint32_t buffer_size) // response from server (single line)
{
   U_TRACE(0, "USocketExt::readLineReply(%p,%p,%u)", sk, buffer, buffer_size)

   U_INTERNAL_ASSERT(sk->isConnected())

   int i, r = 0;

   do {
      int count = buffer_size - r;

      i = sk->recv(buffer + r, count);

      if (sk->checkIO(i) == false) U_RETURN(USocket::BROKEN);

      r += i;
      }
   while (buffer[r-1] != '\n');

   buffer[r] = '\0';

   U_RETURN(r);
}

int USocketExt::readMultilineReply(USocket* sk, char* buffer, uint32_t buffer_size) // response from server (multi line)
{
   U_TRACE(0, "USocketExt::readMultilineReply(%p,%p,%u)", sk, buffer, buffer_size)

   U_INTERNAL_ASSERT(sk->isConnected())

   int r = 0, response;

   do {
      r = readLineReply(sk, buffer + r, buffer_size - r);

      if (r == USocket::BROKEN) U_RETURN(USocket::BROKEN);

      response = atoi(buffer);
      }
   while (parseCommandResponse(buffer, r, response));

   U_RETURN(response);
}

// SERVICES

UString USocketExt::getNetworkDevice(const char* exclude)
{
   U_TRACE(1, "USocketExt::getNetworkDevice(%S)", exclude)

   UString result(100U);

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   FILE* route = (FILE*) U_SYSCALL(fopen, "%S,%S", "/proc/net/route", "r");

   if (U_SYSCALL(fscanf, "%p,%S", route, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s") != EOF) // Skip the first line
      {
      bool found;
      char dev[7], dest[9];

      while (U_SYSCALL(fscanf, "%p,%S", route, "%6s %8s %*s %*s %*s %*s %*s %*s %*s %*s %*s\n", dev, dest) != EOF)
         {
         found = (exclude ? (strncmp(dev, exclude, 6) != 0)   // not the whatever it is
                          : (strcmp(dest, "00000000") == 0)); // default route

         if (found)
            {
            (void) result.assign(dev);

            break;
            }
         }
      }

   (void) U_SYSCALL(fclose, "%p", route);
#endif

   U_RETURN_STRING(result);
}

bool USocketExt::getARPCache(UString& cache, UVector<UString>& vec)
{
   U_TRACE(0, "USocketExt::getARPCache(%.*S,%p)", U_STRING_TO_TRACE(cache), &vec)

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   /*
   FILE* arp = (FILE*) U_SYSCALL(fopen, "%S,%S", "/proc/net/arp", "r");

   // ------------------------------------------------------------------------------
   // Skip the first line
   // ------------------------------------------------------------------------------
   // IP address       HW type     Flags       HW address            Mask     Device
   // 192.168.253.1    0x1         0x2         00:14:a5:6e:9c:cb     *        ath0
   // 10.30.1.131      0x1         0x2         00:16:ec:fb:46:da     *        eth0
   // ------------------------------------------------------------------------------

   if (U_SYSCALL(fscanf, "%p,%S", arp, "%*s %*s %*s %*s %*s %*s %*s %*s %*s") != EOF)
      {
      char _ip[16];

      while (U_SYSCALL(fscanf, "%p,%S", arp, "%15s %*s %*s %*s %*s %*s\n", _ip) != EOF)
         {
         UString item((void*)_ip);

         vec.push_back(item);
         }
      }

   (void) U_SYSCALL(fclose, "%p", arp);
   */

   UString content = UFile::getSysContent("/proc/net/arp");

   if (cache != content)
      {
      UString item;
      UVector<UString> vec_row(content, '\n'), vec_entry(6);

      vec.clear();

      cache = content;

      for (uint32_t i = 1, n = vec_row.size(); i < n; ++i) // Skip the first line
         {
         // ------------------------------------------------------------------------------
         // IP address       HW type     Flags       HW address            Mask     Device
         // ------------------------------------------------------------------------------
         // 192.168.253.1    0x1         0x2         00:14:a5:6e:9c:cb     *        ath0
         // 10.30.1.131      0x1         0x2         00:16:ec:fb:46:da     *        eth0
         // ------------------------------------------------------------------------------

         (void) vec_entry.split(vec_row[i]);

         item = vec_entry[0]; // ip

         U_INTERNAL_ASSERT(item)

         vec.push_back(item);

         item = vec_entry[3]; // mac

         U_INTERNAL_ASSERT(item)

         vec.push_back(item);

         item = vec_entry[5]; // dev

         U_INTERNAL_ASSERT(item)

         vec.push_back(item);

         vec_entry.clear();
         }

      U_RETURN(true);
      }
#endif

   U_RETURN(false);
}

UString USocketExt::getNetworkInterfaceName(const char* ip, uint32_t ip_len)
{
   U_TRACE(0, "USocketExt::getNetworkInterfaceName(%.*S,%u)", ip_len, ip, ip_len)

   U_INTERNAL_ASSERT(u_isIPv4Addr(ip, ip_len))

   UString result(100U);

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   /*
   FILE* arp = (FILE*) U_SYSCALL(fopen, "%S,%S", "/proc/net/arp", "r");

   // ------------------------------------------------------------------------------
   // Skip the first line
   // ------------------------------------------------------------------------------
   // IP address       HW type     Flags       HW address            Mask     Device
   // 192.168.253.1    0x1         0x2         00:14:a5:6e:9c:cb     *        ath0
   // 10.30.1.131      0x1         0x2         00:16:ec:fb:46:da     *        eth0
   // ------------------------------------------------------------------------------

   if (U_SYSCALL(fscanf, "%p,%S", arp, "%*s %*s %*s %*s %*s %*s %*s %*s %*s") != EOF)
      {
      char _ip[16], dev[16];

      while (U_SYSCALL(fscanf, "%p,%S", arp, "%15s %*s %*s %*s %*s %15s\n", _ip, dev) != EOF)
         {
         if (strcmp(ip, _ip) == 0)
            {
            (void) result.assign(dev);

            break;
            }
         }
      }

   (void) U_SYSCALL(fclose, "%p", arp);
   */

   UString content;
   UVector<UString> vec;

   if (getARPCache(content, vec))
      {
      for (uint32_t i = 0, n = vec.size(); i < n; i += 3)
         {
         if (vec[i].equal(ip, ip_len)) U_RETURN_STRING(vec[i+2].copy());
         }
      }
#endif

   U_RETURN_STRING(result);
}

UString USocketExt::getMacAddress(const char* ip, uint32_t ip_len)
{
   U_TRACE(0, "USocketExt::getMacAddress(%.*S,%u)", ip_len, ip, ip_len)

   U_INTERNAL_ASSERT(u_isIPv4Addr(ip, ip_len))

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   /*
   FILE* arp = (FILE*) U_SYSCALL(fopen, "%S,%S", "/proc/net/arp", "r");

   // ------------------------------------------------------------------------------
   // Skip the first line
   // ------------------------------------------------------------------------------
   // IP address       HW type     Flags       HW address            Mask     Device
   // 192.168.253.1    0x1         0x2         00:14:a5:6e:9c:cb     *        ath0
   // 10.30.1.131      0x1         0x2         00:16:ec:fb:46:da     *        eth0
   // ------------------------------------------------------------------------------

   if (U_SYSCALL(fscanf, "%p,%S", arp, "%*s %*s %*s %*s %*s %*s %*s %*s %*s") != EOF)
      {
      char ip[16], hw[18];

      while (U_SYSCALL(fscanf, "%p,%S", arp, "%15s %*s %*s %17s %*s %*s\n", ip, hw) != EOF)
         {
         if (strncmp(device_or_ip, ip, sizeof(ip)) == 0)
            {
            (void) result.assign(hw);

            break;
            }
         }
      }

   (void) U_SYSCALL(fclose, "%p", arp);
   */

   UString content;
   UVector<UString> vec;

   if (getARPCache(content, vec))
      {
      for (uint32_t i = 0, n = vec.size(); i < n; i += 3)
         {
         if (vec[i].equal(ip, ip_len)) U_RETURN_STRING(vec[i+1].copy());
         }
      }
#endif

   U_RETURN_STRING(*UString::str_without_mac);
}

UString USocketExt::getMacAddress(int fd, const char* device)
{
   U_TRACE(1, "USocketExt::getMacAddress(%d,%S)", fd, device)

   U_INTERNAL_ASSERT_POINTER(device)

   UString result(100U);

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   U_INTERNAL_ASSERT(fd != -1)

   struct ifreq ifr;

   (void) u__strncpy(ifr.ifr_name, device, IFNAMSIZ-1);

   (void) U_SYSCALL(ioctl, "%d,%d,%p", fd, SIOCGIFHWADDR, &ifr);

   char* hwaddr = ifr.ifr_hwaddr.sa_data;

   result.snprintf("%02x:%02x:%02x:%02x:%02x:%02x",
                   hwaddr[0] & 0xFF,
                   hwaddr[1] & 0xFF,
                   hwaddr[2] & 0xFF,
                   hwaddr[3] & 0xFF,
                   hwaddr[4] & 0xFF,
                   hwaddr[5] & 0xFF);
#endif

   U_RETURN_STRING(result);
}

UString USocketExt::getIPAddress(int fd, const char* device)
{
   U_TRACE(1, "USocketExt::getIPAddress(%d,%S)", fd, device)

   U_INTERNAL_ASSERT(fd != -1)
   U_INTERNAL_ASSERT_POINTER(device)

   UString result(100U);

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   struct ifreq ifr;

   (void) u__strncpy(ifr.ifr_name, device, IFNAMSIZ-1);

   /* Get the IP address of the interface */

   (void) U_SYSCALL(ioctl, "%d,%d,%p", fd, SIOCGIFADDR, &ifr);

   uusockaddr addr;

   U_MEMCPY(&addr, &ifr.ifr_addr, sizeof(struct sockaddr));

   U_INTERNAL_ASSERT_EQUALS(addr.psaIP4Addr.sin_family, AF_INET)

   (void) U_SYSCALL(inet_ntop, "%d,%p,%p,%u", AF_INET, &(addr.psaIP4Addr.sin_addr), result.data(), INET_ADDRSTRLEN);

   result.size_adjust();
#endif

   U_RETURN_STRING(result);
}

UString USocketExt::getNetworkAddress(int fd, const char* device)
{
   U_TRACE(1, "USocketExt::getNetworkAddress(%d,%S)", fd, device)

   U_INTERNAL_ASSERT(fd != -1)
   U_INTERNAL_ASSERT_POINTER(device)

   UString result(100U);

#if !defined(_MSWINDOWS_) && defined(HAVE_SYS_IOCTL_H)
   struct ifreq ifaddr, ifnetmask;

   (void) u__strncpy(   ifaddr.ifr_name, device, IFNAMSIZ-1);
   (void) u__strncpy(ifnetmask.ifr_name, device, IFNAMSIZ-1);

   // retrieve the IP address and subnet mask

   (void) U_SYSCALL(ioctl, "%d,%d,%p", fd,    SIOCGIFADDR, &ifaddr);
   (void) U_SYSCALL(ioctl, "%d,%d,%p", fd, SIOCGIFNETMASK, &ifnetmask);

   // compute the current network value from the address and netmask

   int network;
   uusockaddr addr, netmask;

   U_MEMCPY(&addr,    &ifaddr.ifr_addr,       sizeof(struct sockaddr));
   U_MEMCPY(&netmask, &ifnetmask.ifr_netmask, sizeof(struct sockaddr));

   U_INTERNAL_ASSERT_EQUALS(addr.psaIP4Addr.sin_family,    AF_INET)
   U_INTERNAL_ASSERT_EQUALS(netmask.psaIP4Addr.sin_family, AF_INET)

   network =     addr.psaIP4Addr.sin_addr.s_addr &
              netmask.psaIP4Addr.sin_addr.s_addr;

   /*
   result.snprintf("%d.%d.%d.%d", (network       & 0xFF),
                                  (network >>  8 & 0xFF),
                                  (network >> 16 & 0xFF),
                                  (network >> 24 & 0xFF));
   */

   (void) U_SYSCALL(inet_ntop, "%d,%p,%p,%u", AF_INET, &network, result.data(), INET_ADDRSTRLEN);

   result.size_adjust();
#endif

   U_RETURN_STRING(result);
}

#ifdef USE_C_ARES
int   USocketExt::resolv_status;
char  USocketExt::resolv_hostname[INET6_ADDRSTRLEN];
void* USocketExt::resolv_channel;

U_NO_EXPORT void USocketExt::callbackResolv(void* arg, int status, int timeouts, struct hostent* phost)
{
   U_TRACE(0, "USocketExt::callbackResolv(%p,%d,%d,%p)", arg, status, timeouts, phost)

   U_INTERNAL_ASSERT_POINTER(resolv_channel)
   U_INTERNAL_ASSERT_EQUALS(resolv_status, ARES_ENODATA)

   resolv_status = status;

   if (phost)
      {
#  ifdef HAVE_INET_NTOP
      (void) U_SYSCALL(inet_ntop, "%d,%p,%p,%u", phost->h_addrtype, phost->h_addr_list[0], resolv_hostname, INET6_ADDRSTRLEN);
#  else
      char* result = U_SYSCALL(inet_ntoa, "%u", *((struct in_addr*)phost->h_addr_list[0]));

      if (result) u__strcpy(resolv_hostname, result);
#  endif

      U_INTERNAL_DUMP("Found address name %s (%s) - status %d timeouts %d", phost->h_name, resolv_hostname, status, timeouts)
      }
}

void USocketExt::waitResolv()
{
   U_TRACE(1, "USocketExt::waitResolv()")

   U_INTERNAL_ASSERT_POINTER(resolv_channel)

   while (resolv_status == ARES_ENODATA)
      {
      int nfds;
      struct timeval tv;
      struct timeval* tvp;
      fd_set read_fds, write_fds;

      FD_ZERO( &read_fds);
      FD_ZERO(&write_fds);

      nfds = U_SYSCALL(ares_fds, "%p,%p,%p", (ares_channel)resolv_channel, &read_fds, &write_fds);

      if (nfds <= 0) break;

      tvp = (struct timeval*) U_SYSCALL(ares_timeout, "%p,%p,%p", (ares_channel)resolv_channel, 0, &tv);

      (void) U_SYSCALL(select, "%d,%p,%p,%p,%p", nfds, &read_fds, &write_fds, 0, tvp);

      U_SYSCALL_VOID(ares_process, "%p,%p,%p", (ares_channel)resolv_channel, &read_fds, &write_fds);

      U_INTERNAL_DUMP("status %d", resolv_status)
      }
}

void USocketExt::startResolv(const char* name, int family)
{
   U_TRACE(1, "USocketExt::startResolv(%S,%d)", name, family)

   if (resolv_channel == 0)
      {
      int status = U_SYSCALL(ares_library_init, "%d", ARES_LIB_INIT_ALL);

      if (status != ARES_SUCCESS) U_ERROR("ares_library_init() failed: %s", ares_strerror(status));

      struct ares_options options;

      status = U_SYSCALL(ares_init_options, "%p,%p,%d", (ares_channel*)&resolv_channel, &options, 0);

      if (status != ARES_SUCCESS) U_ERROR("ares_init_options() failed: %s", ares_strerror(status));
      }

   resolv_status = ARES_ENODATA;

   U_SYSCALL_VOID(ares_gethostbyname, "%p,%S,%d,%p,%p", (ares_channel)resolv_channel, name, family, &USocketExt::callbackResolv, 0);
}
#endif

#if defined(LINUX) || defined(__LINUX__) || defined(__linux__)
#  include <linux/types.h>
#  include <linux/rtnetlink.h>
#endif

UString USocketExt::getGatewayAddress(const char* network, uint32_t network_len)
{
   U_TRACE(1, "USocketExt::getGatewayAddress(%.*S,%u)", network_len, network, network_len)

   UString result(100U);

   // Ex: ip route show to exact 192.168.1.0/24

#if defined(LINUX) || defined(__LINUX__) || defined(__linux__)
   static int sock;

   if (sock == 0) sock = USocket::socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);

   if (sock != -1)
      {
      char msgBuf[4096];

      (void) U_SYSCALL(memset, "%p,%d,%u", msgBuf, 0, 4096);

      /*
      struct nlmsghdr {
         __u32 nlmsg_len;    // Length of message including header
         __u16 nlmsg_type;   // Type of message content
         __u16 nlmsg_flags;  // Additional flags
         __u32 nlmsg_seq;    // Sequence number
         __u32 nlmsg_pid;    // PID of the sending process
      };
      */

      // point the header and the msg structure pointers into the buffer

      union uunlmsghdr {
         char*            p;
         struct nlmsghdr* h;
      };

      union uunlmsghdr nlMsg = { &msgBuf[0] };

      // Fill in the nlmsg header

      nlMsg.h->nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg)); // Length of message (28)
      nlMsg.h->nlmsg_type  = RTM_GETROUTE;                       // Get the routes from kernel routing table
      nlMsg.h->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;         // The message is a request for dump
      nlMsg.h->nlmsg_seq   = 0;                                  // Sequence of the message packet
      nlMsg.h->nlmsg_pid   = u_pid;                              // PID of process sending the request

      // Send the request

      if (U_SYSCALL(send, "%d,%p,%u,%u", sock, CAST(nlMsg.h), nlMsg.h->nlmsg_len, 0) == (ssize_t)nlMsg.h->nlmsg_len)
         {
         // Read the response

         int readLen;
         uint32_t msgLen = 0;
         char* bufPtr = msgBuf;
         union uunlmsghdr nlHdr;

         do {
            // Receive response from the kernel

            if ((readLen = U_SYSCALL(recv, "%d,%p,%u,%d", sock, CAST(bufPtr), 4096 - msgLen, 0)) < 0) break;

            nlHdr.p = bufPtr;

            // Check if the header is valid

            if ((NLMSG_OK(nlHdr.h, (uint32_t)readLen) == 0) || (nlHdr.h->nlmsg_type == NLMSG_ERROR)) break;

            // Check if it is the last message

            U_INTERNAL_DUMP("nlmsg_type = %u nlmsg_seq = %u nlmsg_pid = %u nlmsg_flags = %B",
                             nlHdr.h->nlmsg_type, nlHdr.h->nlmsg_seq, nlHdr.h->nlmsg_pid, nlHdr.h->nlmsg_flags)

            if (nlHdr.h->nlmsg_type == NLMSG_DONE) break;
            else
               {
               // Else move the pointer to buffer appropriately

               bufPtr += readLen;
               msgLen += readLen;
               }

            // Check if it is a multi part message

            if ((nlHdr.h->nlmsg_flags & NLM_F_MULTI) == 0) break;
            }
         while ((nlHdr.h->nlmsg_seq != 1) || (nlHdr.h->nlmsg_pid != (uint32_t)u_pid));

         U_INTERNAL_DUMP("msgLen = %u readLen = %u", msgLen, readLen)

         // Parse the response

         int rtLen;
         char* dst;
         char dstMask[32];
         struct rtmsg* rtMsg;
         struct rtattr* rtAttr;
         char ifName[IF_NAMESIZE];
         struct in_addr dstAddr, srcAddr, gateWay;

         for (; NLMSG_OK(nlMsg.h,msgLen); nlMsg.h = NLMSG_NEXT(nlMsg.h,msgLen))
            {
            rtMsg = (struct rtmsg*) NLMSG_DATA(nlMsg.h);

            U_INTERNAL_DUMP("rtMsg = %p msgLen = %u rtm_family = %u rtm_table = %u", rtMsg, msgLen, rtMsg->rtm_family, rtMsg->rtm_table)

            /*
            #define AF_INET   2 // IP protocol family
            #define AF_INET6 10 // IP version 6
            */

            if ((rtMsg->rtm_family != AF_INET)) continue; // If the route is not for AF_INET then continue

            /* Reserved table identifiers

            enum rt_class_t {
               RT_TABLE_UNSPEC=0,
               RT_TABLE_COMPAT=252,
               RT_TABLE_DEFAULT=253,
               RT_TABLE_MAIN=254,
               RT_TABLE_LOCAL=255,
               RT_TABLE_MAX=0xFFFFFFFF }; */

            if ((rtMsg->rtm_table != RT_TABLE_MAIN)) continue; // If the route does not belong to main routing table then continue

            ifName[0] = '\0';
            dstAddr.s_addr = srcAddr.s_addr = gateWay.s_addr = 0;

            // get the rtattr field

            rtAttr = (struct rtattr*) RTM_RTA(rtMsg);
            rtLen  = RTM_PAYLOAD(nlMsg.h);

            for (; RTA_OK(rtAttr,rtLen); rtAttr = RTA_NEXT(rtAttr,rtLen))
               {
               U_INTERNAL_DUMP("rtAttr = %p rtLen = %u rta_type = %u rta_len = %u", rtAttr, rtLen, rtAttr->rta_type, rtAttr->rta_len)

               /* Routing message attributes

               struct rtattr {
                  unsigned short rta_len;  // Length of option
                  unsigned short rta_type; //   Type of option
               // Data follows
               };

               enum rtattr_type_t {
                  RTA_UNSPEC,    // 0
                  RTA_DST,       // 1
                  RTA_SRC,       // 2
                  RTA_IIF,       // 3
                  RTA_OIF,       // 4
                  RTA_GATEWAY,   // 5
                  RTA_PRIORITY,  // 6
                  RTA_PREFSRC,   // 7
                  RTA_METRICS,   // 8
                  RTA_MULTIPATH, // 9
                  RTA_PROTOINFO, // no longer used
                  RTA_FLOW,      // 11
                  RTA_CACHEINFO, // 12
                  RTA_SESSION,   // no longer used
                  RTA_MP_ALGO,   // no longer used
                  RTA_TABLE,     // 15
                  RTA_MARK,      // 16
                  __RTA_MAX }; */

               switch (rtAttr->rta_type)
                  {
                  case RTA_OIF:     (void) if_indextoname(*(unsigned*)RTA_DATA(rtAttr), ifName);   break;
                  case RTA_GATEWAY: U_MEMCPY(&gateWay, RTA_DATA(rtAttr), sizeof(struct in_addr)); break;
                  case RTA_PREFSRC: U_MEMCPY(&srcAddr, RTA_DATA(rtAttr), sizeof(struct in_addr)); break;
                  case RTA_DST:     U_MEMCPY(&dstAddr, RTA_DATA(rtAttr), sizeof(struct in_addr)); break;
                  }
               }

            U_DUMP("ifName = %S dstAddr = %S rtMsg->rtm_dst_len = %u srcAddr = %S gateWay = %S", ifName,
                        UIPAddress::toString(dstAddr.s_addr).data(), rtMsg->rtm_dst_len,
                        UIPAddress::toString(srcAddr.s_addr).data(),
                        UIPAddress::toString(gateWay.s_addr).data())

            dst = U_SYSCALL(inet_ntoa, "%u", dstAddr);

            if (u__snprintf(dstMask, sizeof(dstMask), "%s/%u", dst, rtMsg->rtm_dst_len) == network_len &&
                    strncmp(dstMask, network, network_len) == 0)
               {
               if (gateWay.s_addr)
                  {
                  (void) U_SYSCALL(inet_ntop, "%d,%p,%p,%u", AF_INET, &gateWay, result.data(), result.capacity());
                  }
               else
                  {
                  U_INTERNAL_ASSERT_MAJOR(srcAddr.s_addr, 0)

                  (void) U_SYSCALL(inet_ntop, "%d,%p,%p,%u", AF_INET, &srcAddr, result.data(), result.capacity());
                  }

               result.size_adjust();

               break;
               }
            }
         }
      }
#endif

   U_RETURN_STRING(result);
}
