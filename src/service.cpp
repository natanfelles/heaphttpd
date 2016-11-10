/*
	Copyright (c) openheap, uplusware
	uplusware@gmail.com
*/

#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <semaphore.h>
#include <mqueue.h>
#include <pthread.h>
#include <queue>
#include <sys/syscall.h>
#define gettid() syscall(__NR_gettid)
#include "service.h"
#include "session.h"
#include "cache.h"
#include "pool.h"
#include "util/trace.h"

typedef struct
{
    
	int sockfd;
	string client_ip;
	BOOL https;
	BOOL http2;
    string ca_crt_root;
    string ca_crt_server;
    string ca_password;
    string ca_key_server;
    BOOL client_cer_check;
	memory_cache* cache;
	ServiceObjMap* srvobjmap;
} SESSION_PARAM;

enum CLIENT_PARAM_CTRL{
	SessionParamData = 0,
	SessionParamExt,
	SessionParamQuit
};

typedef struct {
	CLIENT_PARAM_CTRL ctrl;
	char client_ip[128];
    BOOL https;
	BOOL http2;
    char ca_crt_root[256];
    char ca_crt_server[256];
    char ca_password[256];
    char ca_key_server[256];
    BOOL client_cer_check;
} CLIENT_PARAM;

int SEND_FD(int sfd, int fd_file, CLIENT_PARAM* param) 
{
	struct msghdr msg;  
    struct iovec iov[1];  
    union{  
        struct cmsghdr cm;  
        char control[CMSG_SPACE(sizeof(int))];  
    }control_un;  
    struct cmsghdr *cmptr;     
    msg.msg_control = control_un.control;   
    msg.msg_controllen = sizeof(control_un.control);  
    cmptr = CMSG_FIRSTHDR(&msg);  
    cmptr->cmsg_len = CMSG_LEN(sizeof(int));
    cmptr->cmsg_level = SOL_SOCKET;   
    cmptr->cmsg_type = SCM_RIGHTS;
    *((int*)CMSG_DATA(cmptr)) = fd_file;
    msg.msg_name = NULL;  
    msg.msg_namelen = 0;  
    iov[0].iov_base = param;  
    iov[0].iov_len = sizeof(CLIENT_PARAM);  
    msg.msg_iov = iov;  
    msg.msg_iovlen = 1;

    return sendmsg(sfd, &msg, 0); 

}

int RECV_FD(int sfd, int* fd_file, CLIENT_PARAM* param) 
{
    struct msghdr msg;  
    struct iovec iov[1];  
    int nrecv;  
    union{
		struct cmsghdr cm;  
		char control[CMSG_SPACE(sizeof(int))];  
    }control_un;  
    struct cmsghdr *cmptr;  
    msg.msg_control = control_un.control;  
    msg.msg_controllen = sizeof(control_un.control);
    msg.msg_name = NULL;  
    msg.msg_namelen = 0;  

    iov[0].iov_base = param;  
    iov[0].iov_len = sizeof(CLIENT_PARAM);  
    msg.msg_iov = iov;  
    msg.msg_iovlen = 1;

    if((nrecv = recvmsg(sfd, &msg, 0)) <= 0)  
    {  

        return nrecv;  
    }

    cmptr = CMSG_FIRSTHDR(&msg);  
    if((cmptr != NULL) && (cmptr->cmsg_len == CMSG_LEN(sizeof(int))))  
    {  
        if(cmptr->cmsg_level != SOL_SOCKET)  
        {  
            printf("control level != SOL_SOCKET/n");  
            exit(-1);  
        }  
        if(cmptr->cmsg_type != SCM_RIGHTS)  
        {  
            printf("control type != SCM_RIGHTS/n");  
            exit(-1);  
        } 
        *fd_file = *((int*)CMSG_DATA(cmptr));  
    }  
    else  
    {  
        if(cmptr == NULL)
			printf("null cmptr, fd not passed.\n");  
        else
			printf("message len[%d] if incorrect.\n", cmptr->cmsg_len);  
        *fd_file = -1; // descriptor was not passed  
    }   
    return *fd_file;  
}

static std::queue<SESSION_PARAM*> STATIC_THREAD_POOL_ARG_QUEUE;

static volatile BOOL STATIC_THREAD_POOL_EXIT = TRUE;
static pthread_mutex_t STATIC_THREAD_POOL_MUTEX;
static sem_t STATIC_THREAD_POOL_SEM;
static volatile unsigned int STATIC_THREAD_POOL_SIZE = 0;

/*
OpenSSL example:
	unsigned char vector[] = {
	 6, 's', 'p', 'd', 'y', '/', '1',
	 8, 'h', 't', 't', 'p', '/', '1', '.', '1'
	};
	unsigned int length = sizeof(vector);
*/

typedef struct{
    char* ptr;
    int len;
}tls_alpn;

static int alpn_cb(SSL *ssl,
				const unsigned char **out,
				unsigned char *outlen,
				const unsigned char *in,
				unsigned int inlen,
				void *arg)
{
    int ret = SSL_TLSEXT_ERR_NOACK;
    *out = NULL;
	*outlen = 0;
    
	unsigned char* p = (unsigned char*)in;
	while(inlen > 0 && in && (p - in) < inlen)
	{
		int len = p[0];
		p++;
        
		/*for(int x = 0; x < len; x++)
		{
			printf("%c", p[x]);
		}
		printf("\n");*/
        
		if(len == 2 && memcmp(p, "h2", 2) == 0)
		{
            BOOL* pIsHttp2 = (BOOL*)arg;
            *pIsHttp2 = TRUE;
            *out = p;
            *outlen = len;
            ret = SSL_TLSEXT_ERR_OK;
            break;
		}
        else if(len == 8 && memcmp(p, "http/1.1", 8) == 0)
        {
            BOOL* pIsHttp2 = (BOOL*)arg;
            *pIsHttp2 = FALSE;
            *out = p;
            *outlen = len;
            ret = SSL_TLSEXT_ERR_OK;
            break;
        }
		p = p + len;
	}
    
	return ret;
}

static void SESSION_HANDLING(SESSION_PARAM* session_param)
{
	BOOL isHttp2 = FALSE;
	Session* pSession = NULL;
    SSL* ssl = NULL;
    int ssl_rc = -1;
    BOOL bSSLAccepted;
	SSL_CTX* ssl_ctx = NULL;
	X509* client_cert = NULL;
    if(session_param->https == TRUE)
	{
		SSL_METHOD* meth;
#ifdef OPENSSL_V_1_2
	    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, NULL);
        meth = (SSL_METHOD*)TLSv1_2_server_method();
#else
        SSL_load_error_strings();
		OpenSSL_add_ssl_algorithms();
        if(session_param->http2)
            meth = (SSL_METHOD*)SSLv23_server_method();
        else
            meth = (SSL_METHOD*)TLSv1_2_server_method();
#endif /* TLSV1_2_SUPPORT */
		ssl_ctx = SSL_CTX_new(meth);
		if(!ssl_ctx)
		{
			fprintf(stderr, "SSL_CTX_use_certificate_file: %s\n", ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl3;
		}
		
		if(session_param->client_cer_check)
		{
    		SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    		SSL_CTX_set_verify_depth(ssl_ctx, 4);
		}
		SSL_CTX_load_verify_locations(ssl_ctx, session_param->ca_crt_root.c_str(), NULL);
		if(SSL_CTX_use_certificate_file(ssl_ctx, session_param->ca_crt_server.c_str(), SSL_FILETYPE_PEM) <= 0)
		{
			fprintf(stderr, "SSL_CTX_use_certificate_file: %s\n", ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl3;
		}
		//printf("[%s]\n", session_param->ca_password.c_str());
		SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, (char*)session_param->ca_password.c_str());
		if(SSL_CTX_use_PrivateKey_file(ssl_ctx, session_param->ca_key_server.c_str(), SSL_FILETYPE_PEM) <= 0)
		{
			fprintf(stderr, "SSL_CTX_use_PrivateKey_file: %s\n", ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl3;

		}
		if(!SSL_CTX_check_private_key(ssl_ctx))
		{
			fprintf(stderr, "SSL_CTX_check_private_key: %s\n", ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl3;
		}
		if(session_param->http2)
            ssl_rc = SSL_CTX_set_cipher_list(ssl_ctx, "ECDHE-RSA-AES128-GCM-SHA256:ALL");
        else
            ssl_rc = SSL_CTX_set_cipher_list(ssl_ctx, "ALL");
        if(ssl_rc == 0)
        {
            fprintf(stderr, "SSL_CTX_set_cipher_list: %s\n", ERR_error_string(ERR_get_error(),NULL));
            goto clean_ssl3;
        }
		SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
#ifdef OPENSSL_V_1_2		
		if(session_param->http2)
			SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_cb, &isHttp2);
#endif /* OPENSSL_V_1_2 */		
		ssl = SSL_new(ssl_ctx);
		if(!ssl)
		{
			fprintf(stderr, "SSL_new: %s\n", ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl2;
		}
		ssl_rc = SSL_set_fd(ssl, session_param->sockfd);
        if(ssl_rc == 0)
        {
            fprintf(stderr, "SSL_set_fd: %s\n", ERR_error_string(ERR_get_error(),NULL));
            goto clean_ssl2;
        }
        if(session_param->http2)
            ssl_rc = SSL_set_cipher_list(ssl, "ECDHE-RSA-AES128-GCM-SHA256:ALL");
        else
            ssl_rc = SSL_set_cipher_list(ssl, "ALL");
        if(ssl_rc == 0)
        {
            fprintf(stderr, "SSL_set_cipher_list: %s\n", ERR_error_string(ERR_get_error(),NULL));
            goto clean_ssl2;
        }
re_ssl_accept:        
        ssl_rc = SSL_accept(ssl);
		if(ssl_rc < 0)
		{
            int ret = SSL_get_error(ssl, ssl_rc);
            if(ret == SSL_ERROR_WANT_READ || ret == SSL_ERROR_WANT_WRITE)
            {
                goto re_ssl_accept;
            }
            else if(ret == SSL_ERROR_SYSCALL)
            {
                if(ERR_get_error() == 0)
                {
                    fprintf(stderr, "SSL_accept(%d): shutdown by peer\n", ssl_rc);
                }
                else
                {
                    fprintf(stderr, "SSL_accept(%d): SSL_ERROR_SYSCALL %s\n", ssl_rc, ERR_error_string(ERR_get_error(),NULL));
                }
            }
            else
            {
                fprintf(stderr, "SSL_accept(%d): %s, SSL_get_error: %d\n", ssl_rc, ERR_error_string(ERR_get_error(),NULL), ret);
            }
			goto clean_ssl2;
		}
        else if(ssl_rc = 0)
		{
		    fprintf(stderr, "SSL_accept(%d): %s\n", ssl_rc, ERR_error_string(ERR_get_error(),NULL));
			goto clean_ssl1;
		}
        
        /* printf("HTTP Version: %s\n", isHttp2 ? "HTTP/2" : "HTTP/1.1"); */
        bSSLAccepted = TRUE;
        if(session_param->client_cer_check)
        {
            ssl_rc = SSL_get_verify_result(ssl);
            if(ssl_rc != X509_V_OK)
            {
                fprintf(stderr, "SSL_get_verify_result: %s\n", ERR_error_string(ERR_get_error(),NULL));
                goto clean_ssl1;
            }
        }
		if(session_param->client_cer_check)
		{
			client_cert = SSL_get_peer_certificate(ssl);
			if (client_cert == NULL)
			{
				printf("SSL_get_peer_certificate: %s\n", ERR_error_string(ERR_get_error(),NULL));
				goto clean_ssl1;
			}
		}
	}
	
	pSession = new Session(session_param->srvobjmap, session_param->sockfd, ssl,
        session_param->client_ip.c_str(), client_cert, session_param->https, isHttp2, session_param->cache);
	if(pSession != NULL)
	{
		pSession->Process();
		delete pSession;
	}

clean_ssl1:
    if(client_cert)
        X509_free (client_cert);
    client_cert = NULL;
	if(ssl && bSSLAccepted)
    {
		SSL_shutdown(ssl);
        bSSLAccepted = FALSE;
    }
clean_ssl2:
	if(ssl)
    {
		SSL_free(ssl);
        ssl = NULL;
    }
clean_ssl3:
	if(ssl_ctx)
    {
		SSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
    }
	close(session_param->sockfd);
}

static void INIT_THREAD_POOL_HANDLER()
{
	STATIC_THREAD_POOL_EXIT = TRUE;
	STATIC_THREAD_POOL_SIZE = 0;
	while(!STATIC_THREAD_POOL_ARG_QUEUE.empty())
	{
		STATIC_THREAD_POOL_ARG_QUEUE.pop();
	}
	pthread_mutex_init(&STATIC_THREAD_POOL_MUTEX, NULL);
	sem_init(&STATIC_THREAD_POOL_SEM, 0, 0);
}

static void* START_THREAD_POOL_HANDLER(void* arg)
{
	STATIC_THREAD_POOL_SIZE++;
	struct timespec ts;
	while(STATIC_THREAD_POOL_EXIT)
	{
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1;
		if(sem_timedwait(&STATIC_THREAD_POOL_SEM, &ts) == 0)
		{
			SESSION_PARAM* session_param = NULL;
		
			pthread_mutex_lock(&STATIC_THREAD_POOL_MUTEX);
			if(!STATIC_THREAD_POOL_ARG_QUEUE.empty())
			{
				session_param = STATIC_THREAD_POOL_ARG_QUEUE.front();
				STATIC_THREAD_POOL_ARG_QUEUE.pop();			
			}
			pthread_mutex_unlock(&STATIC_THREAD_POOL_MUTEX);
			if(session_param)
			{
				SESSION_HANDLING(session_param);
				delete session_param;
			}
		}
	}
	STATIC_THREAD_POOL_SIZE--;
	if(arg != NULL)
		delete arg;
	
	pthread_exit(0);
}

static void LEAVE_THREAD_POOL_HANDLER()
{
	printf("LEAVE_THREAD_POOL_HANDLER\n");
	STATIC_THREAD_POOL_EXIT = FALSE;

	pthread_mutex_destroy(&STATIC_THREAD_POOL_MUTEX);
	sem_close(&STATIC_THREAD_POOL_SEM);

    char local_sockfile[256];
    sprintf(local_sockfile, "/tmp/niuhttpd/fastcgi.sock.%05d.%05d", getpid(), gettid());

    unlink(local_sockfile);

	unsigned long timeout = 200;
	while(STATIC_THREAD_POOL_SIZE > 0 && timeout > 0)
	{
		usleep(1000*10);
		timeout--;
	}	
}

static void CLEAR_QUEUE(mqd_t qid)
{
	mq_attr attr;
	struct timespec ts;
	mq_getattr(qid, &attr);
	char* buf = (char*)malloc(attr.mq_msgsize);
	while(1)
	{
		clock_gettime(CLOCK_REALTIME, &ts);
		if(mq_timedreceive(qid, (char*)buf, attr.mq_msgsize, NULL, &ts) == -1)
		{
			break;
		}
	}
	free(buf);
}

//////////////////////////////////////////////////////////////////////////////////
//Worker
Worker::Worker(const char* service_name, int process_seq, int thread_num, int sockfd)
{

	m_sockfd = sockfd;
	m_thread_num = thread_num;
	m_process_seq = process_seq;
	m_service_name = service_name;
#ifdef _WITH_MEMCACHED_    
	m_cache = new memory_cache(m_service_name.c_str(), m_process_seq, CHttpBase::m_work_path.c_str(), CHttpBase::m_memcached_list);
#else
    m_cache = new memory_cache(m_service_name.c_str(), m_process_seq, CHttpBase::m_work_path.c_str());
#endif /* _WITH_MEMCACHED_ */    
	m_cache->load();
}

Worker::~Worker()
{
	if(m_cache)
		delete m_cache;
	m_srvobjmap.ReleaseAll();
}

void Worker::Working()
{
	ThreadPool WorkerPool(m_thread_num, 
	    INIT_THREAD_POOL_HANDLER, START_THREAD_POOL_HANDLER, NULL, LEAVE_THREAD_POOL_HANDLER);
	
	bool bQuit = false;
	while(!bQuit)
	{

		int clt_sockfd;
		CLIENT_PARAM client_param;
		if(RECV_FD(m_sockfd, &clt_sockfd, &client_param)  < 0)
		{
			printf("RECV_FD < 0\n");
			continue;
		}
		if(clt_sockfd < 0)
		{
			fprintf(stderr, "RECV_FD error, clt_sockfd = %d %s %d\n", clt_sockfd, __FILE__, __LINE__);
			bQuit = true;
		}

		if(client_param.ctrl == SessionParamQuit)
		{
			printf("QUIT\n");
			bQuit = true;
		}
		else if(client_param.ctrl == SessionParamExt)
		{
			printf("Reload extensions\n");
			CHttpBase::LoadExtensionList();
		}
		else
		{
			SESSION_PARAM* session_param = new SESSION_PARAM;
			session_param->srvobjmap = &m_srvobjmap;
			session_param->cache = m_cache;
			session_param->sockfd = clt_sockfd;
			session_param->client_ip = client_param.client_ip;
			session_param->https = client_param.https;
			session_param->http2 = client_param.http2;
		    session_param->ca_crt_root = client_param.ca_crt_root;
       		session_param->ca_crt_server = client_param.ca_crt_server;
		    session_param->ca_password = client_param.ca_password;
		    session_param->ca_key_server = client_param.ca_key_server;
		    session_param->client_cer_check = client_param.client_cer_check;

			pthread_mutex_lock(&STATIC_THREAD_POOL_MUTEX);
			STATIC_THREAD_POOL_ARG_QUEUE.push(session_param);
			pthread_mutex_unlock(&STATIC_THREAD_POOL_MUTEX);

			sem_post(&STATIC_THREAD_POOL_SEM);
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////
//Service
Service::Service(Service_Type st)
{
	m_sockfd = -1;
    m_sockfd_ssl = -1;
	m_st = st;
	m_service_name = SVR_NAME_TBL[m_st];
}

Service::~Service()
{

}

void Service::Stop()
{
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	m_service_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_service_sid = sem_open(strsem.c_str(), O_RDWR);
	if(m_service_qid == (mqd_t)-1 || m_service_sid == SEM_FAILED)
	{
		return;
	}	
        
	stQueueMsg qMsg;
	qMsg.cmd = MSG_EXIT;
	sem_wait(m_service_sid);
	mq_send(m_service_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_service_sid);
        if(m_service_qid)
		mq_close(m_service_qid);

	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);
        printf("Stop %s OK\n", SVR_DESP_TBL[m_st]);
}

void Service::ReloadConfig()
{
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	m_service_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_service_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_service_qid == (mqd_t)-1 || m_service_sid == SEM_FAILED)
		return;

	stQueueMsg qMsg;
	qMsg.cmd = MSG_GLOBAL_RELOAD;
	sem_wait(m_service_sid);
	mq_send(m_service_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_service_sid);
	
	if(m_service_qid != (mqd_t)-1)
		mq_close(m_service_qid);
	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);

	printf("Reload %s OK\n", SVR_DESP_TBL[m_st]);
}

void Service::ReloadAccess()
{
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	m_service_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_service_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_service_qid == (mqd_t)-1 || m_service_sid == SEM_FAILED)
		return;

	stQueueMsg qMsg;
	qMsg.cmd = MSG_ACCESS_RELOAD;
	sem_wait(m_service_sid);
	mq_send(m_service_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_service_sid);
	
	if(m_service_qid != (mqd_t)-1)
		mq_close(m_service_qid);
	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);
}

void Service::AppendReject(const char* data)
{
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	m_service_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_service_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_service_qid == (mqd_t)-1 || m_service_sid == SEM_FAILED)
		return;

	stQueueMsg qMsg;
	qMsg.cmd = MSG_REJECT_APPEND;
	strncpy(qMsg.data.reject_ip, data, 255);
	qMsg.data.reject_ip[255] = '\0';

	sem_wait(m_service_sid);
	mq_send(m_service_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_service_sid);
	
	if(m_service_qid != (mqd_t)-1)
		mq_close(m_service_qid);
	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);
}

void Service::ReloadExtension()
{
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	m_service_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_service_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_service_qid == (mqd_t)-1 || m_service_sid == SEM_FAILED)
		return;

	stQueueMsg qMsg;
	qMsg.cmd = MSG_EXTENSION_RELOAD;
	sem_wait(m_service_sid);
	mq_send(m_service_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_service_sid);
	
	if(m_service_qid != (mqd_t)-1)
		mq_close(m_service_qid);
	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);
}

int Service::Accept(int& clt_sockfd, BOOL https, struct sockaddr_storage& clt_addr, socklen_t clt_size)
{
    struct sockaddr_in * v4_addr;
    struct sockaddr_in6 * v6_addr;
        
    char szclientip[INET6_ADDRSTRLEN];
    if (clt_addr.ss_family == AF_INET)
    {
        v4_addr = (struct sockaddr_in*)&clt_addr;
        if(inet_ntop(AF_INET, (void*)&v4_addr->sin_addr, szclientip, INET6_ADDRSTRLEN) == NULL)
        {    
            close(clt_sockfd);
            return 0;
        }
        m_next_process = ntohl(v4_addr->sin_addr.s_addr) % m_work_processes.size();

    }
    else if(clt_addr.ss_family == AF_INET6)
    {
        v6_addr = (struct sockaddr_in6*)&clt_addr;
        if(inet_ntop(AF_INET6, (void*)&v6_addr->sin6_addr, szclientip, INET6_ADDRSTRLEN) == NULL)
        {    
            close(clt_sockfd);
            return 0;
        }
        m_next_process = ntohl(v6_addr->sin6_addr.s6_addr32[3]) % m_work_processes.size(); 

    }
    else
    {
        m_next_process = 0; 
    }
    
    string client_ip = szclientip;
    int access_result;
    if(CHttpBase::m_permit_list.size() > 0)
    {
        access_result = FALSE;
        for(int x = 0; x < CHttpBase::m_permit_list.size(); x++)
        {
            if(strlike(CHttpBase::m_permit_list[x].c_str(), client_ip.c_str()) == TRUE)
            {
                access_result = TRUE;
                break;
            }
        }
        
        for(int x = 0; x < CHttpBase::m_reject_list.size(); x++)
        {
            if( (strlike(CHttpBase::m_reject_list[x].ip.c_str(), (char*)client_ip.c_str()) == TRUE)
                && (time(NULL) < CHttpBase::m_reject_list[x].expire) )
            {
                access_result = FALSE;
                break;
            }
        }
    }
    else
    {
        access_result = TRUE;
        for(int x = 0; x < CHttpBase::m_reject_list.size(); x++)
        {
            if( (strlike(CHttpBase::m_reject_list[x].ip.c_str(), (char*)client_ip.c_str()) == TRUE)
                && (time(NULL) < CHttpBase::m_reject_list[x].expire) )
            {
                access_result = FALSE;
                break;
            }
        }
    }
    
    if(access_result == FALSE)
    {
        close(clt_sockfd);
        return 0;
    }
    else
    {					                    
        char pid_file[1024];
        sprintf(pid_file, "/tmp/niuhttpd/%s_WORKER%d.pid",
            m_service_name.c_str(), m_next_process);
        if(check_pid_file(pid_file) == true) /* The related process had crashed */
        {
            WORK_PROCESS_INFO  wpinfo;
            if (socketpair(AF_UNIX, SOCK_DGRAM, 0, wpinfo.sockfds) < 0)
                fprintf(stderr, "socketpair error, %s %d\n", __FILE__, __LINE__);
            
            int work_pid = fork();
            if(work_pid == 0)
            {
                if(lock_pid_file(pid_file) == false)
                {
                    exit(-1);
                }
                close(wpinfo.sockfds[0]);
                Worker * pWorker = new Worker(m_service_name.c_str(), m_next_process,
                    CHttpBase::m_max_instance_thread_num, wpinfo.sockfds[1]);
                pWorker->Working();
                delete pWorker;
                close(wpinfo.sockfds[1]);
                exit(0);
            }
            else if(work_pid > 0)
            {
                close(wpinfo.sockfds[1]);
                wpinfo.pid = work_pid;
                m_work_processes[m_next_process] = wpinfo;
            }
            else
            {
                return 0;
            }
        }
        
        CLIENT_PARAM client_param;
        strncpy(client_param.client_ip, client_ip.c_str(), 127);
        client_param.client_ip[127] = '\0';
        client_param.https = https;
        client_param.http2 = CHttpBase::m_enablehttp2;
        strncpy(client_param.ca_crt_root, CHttpBase::m_ca_crt_root.c_str(), 255);
        client_param.ca_crt_root[255] = '\0';
        strncpy(client_param.ca_crt_server, CHttpBase::m_ca_crt_server.c_str(), 255);
        client_param.ca_crt_server[255] = '\0';
        strncpy(client_param.ca_password, CHttpBase::m_ca_password.c_str(), 255);
        client_param.ca_password[255] = '\0';
        strncpy(client_param.ca_key_server, CHttpBase::m_ca_key_server.c_str(), 255);
        client_param.ca_key_server[255] = '\0';
        client_param.client_cer_check = CHttpBase::m_client_cer_check;

        client_param.ctrl = SessionParamData;
        SEND_FD(m_work_processes[m_next_process].sockfds[0], clt_sockfd, &client_param);
        close(clt_sockfd);

    }
    
    return 0;
}

int Service::Run(int fd, const char* hostip, unsigned short http_port, unsigned short https_port)
{	
	CUplusTrace uTrace(LOGNAME, LCKNAME);

	m_child_list.clear();
	unsigned int result = 0;
	string strqueue = "/.niuhttpd_";
	strqueue += m_service_name;
	strqueue += "_queue";

	string strsem = "/.niuhttpd_";
	strsem += m_service_name;
	strsem += "_lock";
	
	mq_attr attr;
	attr.mq_maxmsg = 8;
	attr.mq_msgsize = 1448; 
	attr.mq_flags = 0;

	m_service_qid = (mqd_t)-1;
	m_service_sid = SEM_FAILED;
	
	m_service_qid = mq_open(strqueue.c_str(), O_CREAT|O_RDWR, 0644, &attr);
	m_service_sid = sem_open(strsem.c_str(), O_CREAT|O_RDWR, 0644, 1);
	if((m_service_qid == (mqd_t)-1) || (m_service_sid == SEM_FAILED))
	{		
		if(m_service_sid != SEM_FAILED)
			sem_close(m_service_sid);
	
		if(m_service_qid != (mqd_t)-1)
			mq_close(m_service_qid);

		sem_unlink(strsem.c_str());
		mq_unlink(strqueue.c_str());

		result = 1;
		write(fd, &result, sizeof(unsigned int));
		close(fd);
		return -1;
	}
	
	CLEAR_QUEUE(m_service_qid);
	
	BOOL svr_exit = FALSE;
	int qBufLen = attr.mq_msgsize;
	char* qBufPtr = (char*)malloc(qBufLen);

	m_next_process = 0;
	for(int i = 0; i < CHttpBase::m_max_instance_num; i++)
	{
		char pid_file[1024];
		sprintf(pid_file, "/tmp/niuhttpd/%s_WORKER%d.pid", m_service_name.c_str(), i);
		unlink(pid_file);
		WORK_PROCESS_INFO  wpinfo;
		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, wpinfo.sockfds) < 0)
			fprintf(stderr, "socketpair error, %s %d\n", __FILE__, __LINE__);
		int work_pid = fork();
		if(work_pid == 0)
		{

			if(lock_pid_file(pid_file) == false)
			{
				exit(-1);
			}
			close(wpinfo.sockfds[0]);
			Worker* pWorker = new Worker(m_service_name.c_str(), i, CHttpBase::m_max_instance_thread_num, wpinfo.sockfds[1]);
			if(pWorker)
			{
				pWorker->Working();
				delete pWorker;
			}
			close(wpinfo.sockfds[1]);
			exit(0);
		}
		else if(work_pid > 0)
		{
			close(wpinfo.sockfds[1]);
			wpinfo.pid = work_pid;
			m_work_processes.push_back(wpinfo);
		}
		else
		{
			fprintf(stderr, "fork error, work_pid = %d, %S %d\n", work_pid, __FILE__, __LINE__);
		}
	}

	while(!svr_exit)
	{		
		int nFlag;
        if(http_port > 0)
        {
            struct addrinfo hints;
            struct addrinfo *server_addr, *rp;
            
            memset(&hints, 0, sizeof(struct addrinfo));
            hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
            hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
            hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
            hints.ai_protocol = 0;          /* Any protocol */
            hints.ai_canonname = NULL;
            hints.ai_addr = NULL;
            hints.ai_next = NULL;
            
            char szPort[32];
            sprintf(szPort, "%u", http_port);

            int s = getaddrinfo((hostip && hostip[0] != '\0') ? hostip : NULL, szPort, &hints, &server_addr);
            if (s != 0)
            {
               fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
               break;
            }
            
            for (rp = server_addr; rp != NULL; rp = rp->ai_next)
            {
               m_sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
               if (m_sockfd == -1)
                   continue;
               
               nFlag = 1;
               setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, (char*)&nFlag, sizeof(nFlag));
            
               if (bind(m_sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
                   break;                  /* Success */

               close(m_sockfd);
            }
            
            if (rp == NULL)
            {               /* No address succeeded */
                  fprintf(stderr, "Could not bind\n");
                  break;
            }

            freeaddrinfo(server_addr);           /* No longer needed */
            
            nFlag = fcntl(m_sockfd, F_GETFL, 0);
            fcntl(m_sockfd, F_SETFL, nFlag|O_NONBLOCK);
            
            if(listen(m_sockfd, 128) == -1)
            {
                uTrace.Write(Trace_Error, "Service LISTEN error.");
                result = 1;
                write(fd, &result, sizeof(unsigned int));
                close(fd);
                break;
            }
        }
        //SSL
        if(https_port > 0)
        {
            struct addrinfo hints2;
            struct addrinfo *server_addr2, *rp2;
            
            memset(&hints2, 0, sizeof(struct addrinfo));
            hints2.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
            hints2.ai_socktype = SOCK_STREAM; /* Datagram socket */
            hints2.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
            hints2.ai_protocol = 0;          /* Any protocol */
            hints2.ai_canonname = NULL;
            hints2.ai_addr = NULL;
            hints2.ai_next = NULL;
            
            char szPort2[32];
            sprintf(szPort2, "%u", https_port);

            int s = getaddrinfo((hostip && hostip[0] != '\0') ? hostip : NULL, szPort2, &hints2, &server_addr2);
            if (s != 0)
            {
               fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
               break;
            }
            
            for (rp2 = server_addr2; rp2 != NULL; rp2 = rp2->ai_next)
            {
               m_sockfd_ssl = socket(rp2->ai_family, rp2->ai_socktype, rp2->ai_protocol);
               if (m_sockfd_ssl == -1)
                   continue;
               
               nFlag = 1;
               setsockopt(m_sockfd_ssl, SOL_SOCKET, SO_REUSEADDR, (char*)&nFlag, sizeof(nFlag));
            
               if (bind(m_sockfd_ssl, rp2->ai_addr, rp2->ai_addrlen) == 0)
                   break;                  /* Success */

               close(m_sockfd_ssl);
            }
            
            if (rp2 == NULL)
            {     /* No address succeeded */
                  fprintf(stderr, "Could not bind\n");
                  break;
            }

            freeaddrinfo(server_addr2);           /* No longer needed */
            
            nFlag = fcntl(m_sockfd_ssl, F_GETFL, 0);
            fcntl(m_sockfd_ssl, F_SETFL, nFlag|O_NONBLOCK);
            
            if(listen(m_sockfd_ssl, 128) == -1)
            {
                uTrace.Write(Trace_Error, "Security Service LISTEN error.");
                result = 1;
                write(fd, &result, sizeof(unsigned int));
                close(fd);
                break;
            }
        }
        
        if(m_sockfd == -1 && m_sockfd_ssl == -1)
        {
            uTrace.Write(Trace_Error, "Both Service LISTEN error.");
            result = 1;
            write(fd, &result, sizeof(unsigned int));
            close(fd);
            break;
        }
        BOOL accept_ssl_first = FALSE;
		result = 0;
		write(fd, &result, sizeof(unsigned int));
		close(fd);
		fd_set accept_mask;
    	FD_ZERO(&accept_mask);
		struct timeval accept_timeout;
		struct timespec ts;
		stQueueMsg* pQMsg;
		int rc;
		while(1)
		{	
			waitpid(-1, NULL, WNOHANG);

			clock_gettime(CLOCK_REALTIME, &ts);
			rc = mq_timedreceive(m_service_qid, qBufPtr, qBufLen, 0, &ts);

			if( rc != -1)
			{
				pQMsg = (stQueueMsg*)qBufPtr;
				if(pQMsg->cmd == MSG_EXIT)
				{
					for(int j = 0; j < m_work_processes.size(); j++)
					{
						CLIENT_PARAM client_param;
						client_param.ctrl = SessionParamQuit;
						
						SEND_FD(m_work_processes[j].sockfds[0], 0, &client_param);
					}
					svr_exit = TRUE;
					break;
				}
				else if(pQMsg->cmd == MSG_EXTENSION_RELOAD)
				{
					CHttpBase::LoadExtensionList();
					for(int j = 0; j < m_work_processes.size(); j++)
					{
						CLIENT_PARAM client_param;
						client_param.ctrl = SessionParamExt;
						
						SEND_FD(m_work_processes[j].sockfds[0], 0, &client_param);
					}
				}
				else if(pQMsg->cmd == MSG_GLOBAL_RELOAD)
				{
					CHttpBase::UnLoadConfig();
					CHttpBase::LoadConfig();
				}
				else if(pQMsg->cmd == MSG_ACCESS_RELOAD)
				{
					CHttpBase::LoadAccessList();
				}
				else if(pQMsg->cmd == MSG_REJECT_APPEND)
				{
					//firstly erase the expire record
					vector<stReject>::iterator x;
					for(x = CHttpBase::m_reject_list.begin(); x != CHttpBase::m_reject_list.end();)
					{
						if(x->expire < time(NULL))
							CHttpBase::m_reject_list.erase(x);
					}
	
					stReject sr;
					sr.ip = pQMsg->data.reject_ip;
					sr.expire = time(NULL) + 5;
					CHttpBase::m_reject_list.push_back(sr);
				}
			}
			else
			{
				if(errno != ETIMEDOUT && errno != EINTR && errno != EMSGSIZE)
				{
					fprintf(stderr, "mq_timedreceive error, errno = %d, %S %d\n", errno, __FILE__, __LINE__);
					svr_exit = TRUE;
					break;
				}
				
			}
            if(m_sockfd > 0)
                FD_SET(m_sockfd, &accept_mask);
            
            //SSL
            if(m_sockfd_ssl > 0)
                FD_SET(m_sockfd_ssl, &accept_mask);
            
			accept_timeout.tv_sec = 1;
			accept_timeout.tv_usec = 0;
			rc = select((m_sockfd > m_sockfd_ssl ? m_sockfd : m_sockfd_ssl) + 1, &accept_mask, NULL, NULL, &accept_timeout);
			if(rc == -1)
			{	
                sleep(5);
				break;
			}
			else if(rc == 0)
			{
                continue;
			}
			else
			{
				BOOL https= FALSE;
                
				struct sockaddr_storage clt_addr, clt_addr_ssl;
                
				socklen_t clt_size = sizeof(struct sockaddr_storage);
                socklen_t clt_size_ssl = sizeof(struct sockaddr_storage);
				int clt_sockfd = -1;
                int clt_sockfd_ssl = -1;
                
                if(FD_ISSET(m_sockfd, &accept_mask))
                {
                    https = FALSE;
                    clt_sockfd = accept(m_sockfd, (sockaddr*)&clt_addr, &clt_size);

                    if(clt_sockfd < 0)
                    {
                        continue;
                    }
                    if(Accept(clt_sockfd, https, clt_addr, clt_size) < 0)
                        break;
                }
                
                if(FD_ISSET(m_sockfd_ssl, &accept_mask))
                {
                    https = TRUE;
                    clt_sockfd_ssl = accept(m_sockfd_ssl, (sockaddr*)&clt_addr_ssl, &clt_size_ssl);

                    if(clt_sockfd_ssl < 0)
                    {
                        continue;
                    }
                    if(Accept(clt_sockfd_ssl, https, clt_addr_ssl, clt_size_ssl) < 0)
                        break;
                }
			}
		}
		
        if(m_sockfd > 0)
		{
			m_sockfd = -1;
			close(m_sockfd);
		}
        
        if(m_sockfd_ssl > 0)
		{
			m_sockfd_ssl = -1;
			close(m_sockfd_ssl);
		}
	}
	free(qBufPtr);
	if(m_service_qid != (mqd_t)-1)
		mq_close(m_service_qid);
	if(m_service_sid != SEM_FAILED)
		sem_close(m_service_sid);

	mq_unlink(strqueue.c_str());
	sem_unlink(strsem.c_str());

	CHttpBase::UnLoadConfig();
	
	return 0;
}

