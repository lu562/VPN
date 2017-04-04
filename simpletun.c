/**************************************************************************
 * simpletun.c                                                            *
 *                                                                        *
 * A simplistic, simple-minded, naive tunnelling program using tun/tap    *
 * interfaces and TCP. DO NOT USE THIS PROGRAM FOR SERIOUS PURPOSES.      *
 *                                                                        *
 * You have been warned.                                                  *
 *                                                                        *
 * (C) 2010 Davide Brini.                                                 *
 *                                                                        *
 * DISCLAIMER AND WARNING: this is all work in progress. The code is      *
 * ugly, the algorithms are naive, error checking and input validation    *
 * are very basic, and of course there can be bugs. If that's not enough, *
 * the program has not been thoroughly tested, so it might even fail at   *
 * the few simple things it should be supposed to do right.               *
 * Needless to say, I take no responsibility whatsoever for what the      *
 * program might do. The program has been written mostly for learning     *
 * purposes, and can be used in the hope that is useful, but everything   *
 * is to be taken "as is" and without any kind of warranty, implicit or   *
 * explicit. See the file LICENSE for further details.                    *
 *************************************************************************/ 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h> 
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>

/* buffer for reading from tun/tap interface, must be >= 1500 */
#define BUFSIZE 2000   
#define CLIENT 0
#define SERVER 1
#define PORT 30399
#define UDPPORT 30398

#include <openssl/rsa.h>       /* SSLeay stuff */
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>


/* define HOME to be dir for key and cert files... */
#define HOME "./"
/* Make these what you want for cert & key files */
#define CERTF  "server.crt"
#define KEYF    "server.key"
#define CACERT  "ca.crt"
#define CERTF_C "client.crt"
#define KEYF_C "client.key"

#define CHK_NULL(x) if ((x)==NULL) exit (1)
#define CHK_ERR(err,s) if ((err)==-1) { perror(s); exit(1); }
#define CHK_SSL(err) if ((err)==-1) { ERR_print_errors_fp(stderr); exit(2); }

typedef unsigned char byte;
const char hn[] = "SHA256";
int debug;
char *progname;
unsigned char session_key[32];
unsigned char session_iv[16];
int udp_done;
udpdone = 0;
/**************************************************************************
 * tun_alloc: allocates or reconnects to a tun/tap device. The caller     *
 *            must reserve enough space in *dev.                          *
 **************************************************************************/
int tun_alloc(char *dev, int flags) {

  struct ifreq ifr;
  int fd, err;
  char *clonedev = "/dev/net/tun";

  if( (fd = open(clonedev , O_RDWR)) < 0 ) {
    perror("Opening /dev/net/tun");
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));

  ifr.ifr_flags = flags;

  if (*dev) {
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
  }

  if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
    perror("ioctl(TUNSETIFF)");
    close(fd);
    return err;
  }

  strcpy(dev, ifr.ifr_name);

  return fd;
}

/**************************************************************************
 * cread: read routine that checks for errors and exits if an error is    *
 *        returned.                                                       *
 **************************************************************************/
int cread(int fd, char *buf, int n){
  
  int nread;

  if((nread=read(fd, buf, n)) < 0){
    perror("Reading data");
    exit(1);
  }
  return nread;
}

/**************************************************************************
 * cwrite: write routine that checks for errors and exits if an error is  *
 *         returned.                                                      *
 **************************************************************************/
int cwrite(int fd, char *buf, int n){
  
  int nwrite;

  if((nwrite=write(fd, buf, n)) < 0){
    perror("Writing data");
    exit(1);
  }
  return nwrite;
}

/**************************************************************************
 * read_n: ensures we read exactly n bytes, and puts them into "buf".     *
 *         (unless EOF, of course)                                        *
 **************************************************************************/
int read_n(int fd, char *buf, int n) {

  int nread, left = n;

  while(left > 0) {
    if ((nread = cread(fd, buf, left)) == 0){
      return 0 ;      
    }else {
      left -= nread;
      buf += nread;
    }
  }
  return n;  
}

/**************************************************************************
 * do_debug: prints debugging stuff (doh!)                                *
 **************************************************************************/
void do_debug(char *msg, ...){
  
  va_list argp;
  
  if(debug) {
	va_start(argp, msg);
	vfprintf(stderr, msg, argp);
	va_end(argp);
  }
}

/**************************************************************************
 * my_err: prints custom error messages on stderr.                        *
 **************************************************************************/
void my_err(char *msg, ...) {

  va_list argp;
  
  va_start(argp, msg);
  vfprintf(stderr, msg, argp);
  va_end(argp);
}

/**************************************************************************
 * usage: prints usage and exits.                                         *
 **************************************************************************/
void usage(void) {
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "%s -i <ifacename> [-s|-c <serverIP>] [-p <port>] [-u|-a] [-d]\n", progname);
  fprintf(stderr, "%s -h\n", progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "-i <ifacename>: Name of interface to use (mandatory)\n");
  fprintf(stderr, "-s|-c <serverIP>: run in server mode (-s), or specify server address (-c <serverIP>) (mandatory)\n");
  fprintf(stderr, "-p <port>: port to listen on (if run in server mode) or to connect to (in client mode), default 55555\n");
  fprintf(stderr, "-u|-a: use TUN (-u, default) or TAP (-a)\n");
  fprintf(stderr, "-d: outputs debug information while running\n");
  fprintf(stderr, "-h: prints this help text\n");
  exit(1);
}
/**************************************************************************
 * handleErrors: work together with openssl other functions               *
 **************************************************************************/
void handleErrors(void) {
  ERR_print_errors_fp(stderr);
  abort();
}
/**************************************************************************
 * CRYPTO_memcmp: like memcmp(),copied from openssl source code           *
 **************************************************************************/
int CRYPTO_memcmp(const void *in_a, const void *in_b, size_t len) {
  const uint8_t *a = in_a;
  const uint8_t *b = in_b;
  size_t i;
  uint8_t x = 0;

  for (i = 0; i < len; i++) {
    x |= a[i] ^ b[i];
  }

  return x;
}
/**************************************************************************
 encrypt: AES256 encryption                                               *
 **************************************************************************/
int encrypt(unsigned char *plaintext, int plaintext_len, unsigned char *key,
  unsigned char *iv, unsigned char *ciphertext){
    


  EVP_CIPHER_CTX *ctx;
  int len;
  int ciphertext_len;
  /* Initialise the library */
  ERR_load_crypto_strings();
  OpenSSL_add_all_algorithms();
  OPENSSL_config(NULL);

  /* Create and initialise the context */
  if(!(ctx = EVP_CIPHER_CTX_new())) handleErrors();
   /* Initialise the encryption operation. IMPORTANT - ensure you use a key
   * and IV size appropriate for your cipher
   * In this example we are using 256 bit AES (i.e. a 256 bit key). The
   * IV size for *most* modes is the same as the block size. For AES this
   * is 128 bits */
  if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
    handleErrors();

  /* Provide the message to be encrypted, and obtain the encrypted output.
   * EVP_EncryptUpdate can be called multiple times if necessary
   */
  if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
    handleErrors();
  ciphertext_len = len;

  /* Finalise the encryption. Further ciphertext bytes may be written at
   * this stage.
   */
  if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len)) handleErrors();
  ciphertext_len += len;

  /* Clean up */
  EVP_CIPHER_CTX_free(ctx);

  return ciphertext_len;
}
/**************************************************************************
 * decrypt: AES 256 decryption                                            *
 **************************************************************************/
int decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *key,
  unsigned char *iv, unsigned char *plaintext)
{
  EVP_CIPHER_CTX *ctx;

  int len;

  int plaintext_len;

  /* Create and initialise the context */
  if(!(ctx = EVP_CIPHER_CTX_new())) handleErrors();

  /* Initialise the decryption operation. IMPORTANT - ensure you use a key
   * and IV size appropriate for your cipher
   * In this example we are using 256 bit AES (i.e. a 256 bit key). The
   * IV size for *most* modes is the same as the block size. For AES this
   * is 128 bits */
  if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
    handleErrors();

  /* Provide the message to be decrypted, and obtain the plaintext output.
   * EVP_DecryptUpdate can be called multiple times if necessary
   */
  if(1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len))
    handleErrors();
  plaintext_len = len;

  /* Finalise the decryption. Further plaintext bytes may be written at
   * this stage.
   */
  if(1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len)) handleErrors();
  plaintext_len += len;

  /* Clean up */
  EVP_CIPHER_CTX_free(ctx);

  return plaintext_len;
}
/**************************************************************************
 * sign_it:  signing using HMAC                                           *
 **************************************************************************/
int sign_it(const byte* msg, size_t mlen, byte** sig, size_t* slen, EVP_PKEY* pkey)
{
    /* Returned to caller */
    int result = -1;
    
    if(!msg || !mlen || !sig || !pkey) {
        assert(0);
        return -1;
    }
    
    if(*sig)
        OPENSSL_free(*sig);
    
    *sig = NULL;
    *slen = 0;
    
    EVP_MD_CTX* ctx = NULL;
    
    do
    {
        ctx = EVP_MD_CTX_create();
        assert(ctx != NULL);
        if(ctx == NULL) {
            printf("EVP_MD_CTX_create failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        const EVP_MD* md = EVP_get_digestbyname("SHA256");
        assert(md != NULL);
        if(md == NULL) {
            printf("EVP_get_digestbyname failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        int rc = EVP_DigestInit_ex(ctx, md, NULL);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestInit_ex failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        rc = EVP_DigestSignInit(ctx, NULL, md, NULL, pkey);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignInit failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        rc = EVP_DigestSignUpdate(ctx, msg, mlen);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignUpdate failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        size_t req = 0;
        rc = EVP_DigestSignFinal(ctx, NULL, &req);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignFinal failed (1), error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        assert(req > 0);
        if(!(req > 0)) {
            printf("EVP_DigestSignFinal failed (2), error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        *sig = OPENSSL_malloc(req);
        assert(*sig != NULL);
        if(*sig == NULL) {
            printf("OPENSSL_malloc failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        *slen = req;
        rc = EVP_DigestSignFinal(ctx, *sig, slen);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignFinal failed (3), return code %d, error 0x%lx\n", rc, ERR_get_error());
            break; /* failed */
        }
        
        assert(req == *slen);
        if(rc != 1) {
            printf("EVP_DigestSignFinal failed, mismatched signature sizes %ld, %ld", req, *slen);
            break; /* failed */
        }
        
        result = 0;
        
    } while(0);
    
    if(ctx) {
        EVP_MD_CTX_destroy(ctx);
        ctx = NULL;
    }
    
    /* Convert to 0/1 result */
    return !!result;
}
/**************************************************************************
 * verify it: verifying using HMAC                                        *
 **************************************************************************/
int verify_it(const byte* msg, size_t mlen, const byte* sig, size_t slen, EVP_PKEY* pkey)
{
    /* Returned to caller */
    int result = -1;
    
    if(!msg || !mlen || !sig || !slen || !pkey) {
        assert(0);
        return -1;
    }

    EVP_MD_CTX* ctx = NULL;
    
    do
    {
        ctx = EVP_MD_CTX_create();
        assert(ctx != NULL);
        if(ctx == NULL) {
            printf("EVP_MD_CTX_create failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        const EVP_MD* md = EVP_get_digestbyname("SHA256");
        assert(md != NULL);
        if(md == NULL) {
            printf("EVP_get_digestbyname failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        int rc = EVP_DigestInit_ex(ctx, md, NULL);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestInit_ex failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        rc = EVP_DigestSignInit(ctx, NULL, md, NULL, pkey);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignInit failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        rc = EVP_DigestSignUpdate(ctx, msg, mlen);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestSignUpdate failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        byte buff[EVP_MAX_MD_SIZE];
        size_t size = sizeof(buff);
        
        rc = EVP_DigestSignFinal(ctx, buff, &size);
        assert(rc == 1);
        if(rc != 1) {
            printf("EVP_DigestVerifyFinal failed, error 0x%lx\n", ERR_get_error());
            break; /* failed */
        }
        
        assert(size > 0);
        if(!(size > 0)) {
            printf("EVP_DigestSignFinal failed (2)\n");
            break; /* failed */
        }
        
        const size_t m = (slen < size ? slen : size);
        result = !!CRYPTO_memcmp(sig, buff, m);
        
        OPENSSL_cleanse(buff, sizeof(buff));
        
    } while(0);
    
    if(ctx) {
        EVP_MD_CTX_destroy(ctx);
        ctx = NULL;
    }
    
    /* Convert to 0/1 result */
    return !!result;
}
/**************************************************************************
 * udptunnel: udp connection                                               *
 **************************************************************************/
void   udptunnel(int tap_fd_para,struct sockaddr_in remote_para){
  
  unsigned long int tap2net, net2tap;
  tap2net = 0;
  net2tap = 0;
  unsigned char *key = session_key;
  unsigned char *iv = session_iv;
  printf("session key has size %d and session key is :\n",sizeof(session_key));
  int i ;
  for(i = 0;i<sizeof(session_key);i++){
    printf("%02x",session_key[i]);
  }
  printf("\n");
  //unsigned char *key = (unsigned char *)"01234567890123456789012345678901";
  //unsigned char *iv = (unsigned char *)"0123456789012345";

  int maxfd;
  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];
  byte ciphertext[BUFSIZE];
  byte plaintext[BUFSIZE];
  struct sockaddr_in local, remote;
  char remote_ip[16] = "";            /* dotted quad IP string */
  unsigned short int port = PORT;
  int sock_fd, net_fd, optval = 1;
  int tap_fd = tap_fd_para;
  socklen_t remotelen;

  struct sockaddr_in frombuf;
  struct sockaddr *from = (struct sockaddr *) &frombuf;
  size_t fromlen = sizeof(frombuf);

  const size_t sig_len = 32;
  byte *sig = NULL;
  size_t slen = 0;

  if ( (net_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("socket()");
    exit(1);
  }
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(UDPPORT);

  if (bind(net_fd, (struct sockaddr*) &local, sizeof(local)) < 0) {
      perror("bind()");
      exit(1);
    }
  memcpy(&remote, &remote_para, sizeof(remote));
  remote.sin_port = htons(UDPPORT);


  maxfd = (tap_fd > net_fd)?tap_fd:net_fd;

  while(1) {
    
    int ret;
    fd_set rd_set;

    FD_ZERO(&rd_set);
    FD_SET(tap_fd, &rd_set); FD_SET(net_fd, &rd_set);

    ret = select(maxfd + 1, &rd_set, NULL, NULL, NULL);

    if (ret < 0 && errno == EINTR){
      continue;
    }

    if (ret < 0) {
      perror("select()");
      exit(1);
    }

    if(FD_ISSET(tap_fd, &rd_set)&&udp_done) {
      /* data from tun/tap: just read it and write it to the network */
      tap2net= tap2net + 1;      
      nread = cread(tap_fd, buffer, BUFSIZE);      
      do_debug("TAP2NET %lu: Read %d bytes from the tap interface\n", tap2net, nread);
      plength = nread;
      sign_it(buffer, nread, &sig, &slen, EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, key, sizeof(key)));
      assert(slen==sig_len);
      memcpy(buffer+plength, sig, sig_len);

      nwrite = encrypt(buffer, plength+sig_len, key, iv, ciphertext);

      nwrite = sendto(net_fd, ciphertext, nwrite, 0, (struct sockaddr *) &remote, sizeof(struct sockaddr));      
      do_debug("TAP2NET %lu: Written %d bytes to the network\n", tap2net, nwrite);
    }

    if(FD_ISSET(net_fd, &rd_set)) {   
      net2tap++;      
      nread = recvfrom(net_fd, buffer, BUFSIZE, MSG_DONTWAIT, from, &fromlen);      
      do_debug("NET2TAP %lu: Read %d bytes from the network\n", net2tap, nread);   

      nwrite = decrypt(buffer, nread, key, iv, plaintext);
      plength = nwrite - sig_len;

      verify_it(plaintext, plength, plaintext+plength, sig_len, EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, key, sizeof(key)));
      /* now buffer[] contains a full packet or frame, write it into the tun/tap interface */ 
      nwrite = cwrite(tap_fd, plaintext, plength);
      do_debug("NET2TAP %lu: Written %d bytes to the tap interface\n", net2tap, nwrite);
    }
  }
  
  close(net_fd);
}
/**************************************************************************
 * controlchannel: mainly for key agreement and iv exchanging             *
 **************************************************************************/
void controlchannel(SSL *ssl){

  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];
  struct sockaddr_in local, remote;
  char remote_ip[16] = "";            /* dotted quad IP string */
  unsigned short int port = PORT;
  socklen_t remotelen;
  unsigned long int ctrl2net = 0, net2ctrl = 0;
  char selfkey[32];
  char selfiv[16];
  int i,j;


  //key exchange
  RAND_bytes(selfkey, sizeof(selfkey));
  RAND_bytes(selfiv, sizeof(selfiv));

  j = 0;
  for (i=0; i<sizeof(selfkey); i++) {
    buffer[j++] = selfkey[i];
  }
  for (i=0; i<sizeof(selfiv); i++) {
    buffer[j++] = selfiv[i];
  }
  nwrite = SSL_write (ssl, buffer,j); 
  
  nread = SSL_read(ssl,buffer,BUFSIZE);

  for (i=0; i<sizeof(session_key); i++) {
    session_key[i] =buffer[i]^selfkey[i];
  }
  for (i=0; i<sizeof(session_iv); i++) {
    session_iv[i] = buffer[i+sizeof(session_key)]^selfiv[i];
  }
  udp_done = 1;
  printf("key exchange finished\n");
  
  
}

int main(int argc, char *argv[]) {
  
  int tap_fd, option;
  int flags = IFF_TUN;
  char if_name[IFNAMSIZ] = "";
  int maxfd;
  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];

  struct sockaddr_in local, remote;
  char remote_ip[16] = "";            /* dotted quad IP string */
  unsigned short int port = PORT;
  int sock_fd, net_fd, optval = 1;
  socklen_t remotelen;
  int cliserv = -1;    /* must be specified on cmd line */

 


  //ssl things
  int err;
  SSL_CTX* ctx;
  SSL*     ssl;
  X509*    client_cert;
  X509*    server_cert;
  char*    str;
  SSL_METHOD *meth;





  progname = argv[0];
  
  /* Check command line options */
  while((option = getopt(argc, argv, "i:sc:p:uahd")) > 0) {
    switch(option) {
      case 'd':
        debug = 1;
        break;
      case 'h':
        usage();
        break;
      case 'i':
        strncpy(if_name,optarg, IFNAMSIZ-1);
        break;
      case 's':
        cliserv = SERVER;


        break;
      case 'c':
        cliserv = CLIENT;


              strncpy(remote_ip,optarg,15);
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'u':
        flags = IFF_TUN;
        break;
      case 'a':
        flags = IFF_TAP;
        break;
      default:
        my_err("Unknown option %c\n", option);
        usage();
    }
  }

  argv += optind;
  argc -= optind;

  if(argc > 0) {
    my_err("Too many options!\n");
    usage();
  }

  if(*if_name == '\0') {
    my_err("Must specify interface name!\n");
    usage();
  } else if(cliserv < 0) {
    my_err("Must specify client or server mode!\n");
    usage();
  } else if((cliserv == CLIENT)&&(*remote_ip == '\0')) {
    my_err("Must specify server address!\n");
    usage();
  }


  /* initialize tun/tap interface */
  if ( (tap_fd = tun_alloc(if_name, flags | IFF_NO_PI)) < 0 ) {
    my_err("Error connecting to tun/tap interface %s!\n", if_name);
    exit(1);
  }

  do_debug("Successfully connected to interface %s\n", if_name);

  if ( (sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket()");
    exit(1);
  }

  if(cliserv == CLIENT) {
    /* Client, try to connect to server */

    /* assign the destination address */
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = inet_addr(remote_ip);
    remote.sin_port = htons(port);

    /* connection request */
    if (connect(sock_fd, (struct sockaddr*) &remote, sizeof(remote)) < 0) {
      perror("connect()");
      exit(1);
    }


     net_fd = sock_fd;
      /* SSL preliminaries. We keep the certificate and key with the context. */

    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();
    meth = SSLv23_client_method();
    ctx = SSL_CTX_new (meth);
    if (!ctx) {
      ERR_print_errors_fp(stderr);
      exit(2);
    }

    SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER,NULL); /* whether verify the certificate */
    SSL_CTX_load_verify_locations(ctx,CACERT,NULL);
    
    if (SSL_CTX_use_certificate_file(ctx, CERTF_C, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      exit(-2);
    }
    
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF_C, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      exit(-3);
    }

    if (!SSL_CTX_check_private_key(ctx)) {
      printf("Private key does not match the certificate public keyn");
      exit(-4);
    }
  
    ssl = SSL_new (ctx);                         CHK_NULL(ssl);    
    SSL_set_fd (ssl, net_fd);
    err = SSL_connect (ssl);                     CHK_SSL(err);
    net_fd = sock_fd;
    /* Following two steps are optional and not required for
       data exchange to be successful. */
    
    /* Get the cipher - opt */

    printf ("SSL connection using %s\n", SSL_get_cipher (ssl));
    
    /* Get server's certificate (note: beware of dynamic allocation) - opt */

    server_cert = SSL_get_peer_certificate (ssl);       CHK_NULL(server_cert);
    printf ("Server certificate:\n");
    printf("haha\n");
    str = X509_NAME_oneline (X509_get_subject_name (server_cert),0,0);
    CHK_NULL(str);
    printf ("\t subject: %s\n", str);
    OPENSSL_free (str);

    str = X509_NAME_oneline (X509_get_issuer_name  (server_cert),0,0);
    CHK_NULL(str);
    printf ("\t issuer: %s\n", str);
    OPENSSL_free (str);

    /* We could do all sorts of certificate verification stuff here before
       deallocating the certificate. */

    X509_free (server_cert);
      
      do_debug("CLIENT: Connected to server %s\n", inet_ntoa(remote.sin_addr));
      
  } else {
    /* Server, wait for connections */

    /* avoid EADDRINUSE error on bind() */
    if(setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval)) < 0) {
      perror("setsockopt()");
      exit(1);
    }
    
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock_fd, (struct sockaddr*) &local, sizeof(local)) < 0) {
      perror("bind()");
      exit(1);
    }
    
    if (listen(sock_fd, 5) < 0) {
      perror("listen()");
      exit(1);
    }
    
    /* wait for connection request */
    remotelen = sizeof(remote);
    memset(&remote, 0, remotelen);
    if ((net_fd = accept(sock_fd, (struct sockaddr*)&remote, &remotelen)) < 0) {
      perror("accept()");
      exit(1);
    }
        /* SSL preliminaries. We keep the certificate and key with the context. */

    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();
    meth = SSLv23_server_method();
    ctx = SSL_CTX_new (meth);
    if (!ctx) {
      ERR_print_errors_fp(stderr);
      exit(2);
    }

    SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER,NULL); /* whether verify the certificate */
    SSL_CTX_load_verify_locations(ctx,CACERT,NULL);
    if (SSL_CTX_use_certificate_file(ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      exit(3);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
      ERR_print_errors_fp(stderr);
      exit(4);
    }

    if (!SSL_CTX_check_private_key(ctx)) {
      fprintf(stderr,"Private key does not match the certificate public key\n");
      exit(5);
    }
    /* TCP connection is ready. Do server side SSL. */

    ssl = SSL_new (ctx);                           CHK_NULL(ssl);
    SSL_set_fd (ssl, net_fd);
    err = SSL_accept (ssl);                        CHK_SSL(err);
    
    /* Get the cipher - opt */
    
    printf ("SSL connection using %s\n", SSL_get_cipher (ssl));
    
    /* Get client's certificate (note: beware of dynamic allocation) - opt */

    client_cert = SSL_get_peer_certificate (ssl);
    if (client_cert != NULL) {
      printf ("Client certificate:\n");
      
      str = X509_NAME_oneline (X509_get_subject_name (client_cert), 0, 0);
      CHK_NULL(str);
      printf ("\t subject: %s\n", str);
      OPENSSL_free (str);
      
      str = X509_NAME_oneline (X509_get_issuer_name  (client_cert), 0, 0);
      CHK_NULL(str);
      printf ("\t issuer: %s\n", str);
      OPENSSL_free (str);
      
      /* We could do all sorts of certificate verification stuff here before
         deallocating the certificate. */
      
      X509_free (client_cert);
    } else
      printf ("Client does not have certificate.\n");

      do_debug("SERVER: Client connected from %s\n", inet_ntoa(remote.sin_addr));
  }


  controlchannel(ssl);

  //udp tunnel here
  udptunnel(tap_fd,remote);

  
  
  return(0);
}
