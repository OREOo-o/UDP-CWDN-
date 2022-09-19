#pragma comment(lib,"ws2_32.lib") 
#include <stdlib.h> 
#include <time.h> 
#include <WinSock2.h> 
#include <fstream> 
#include <iostream>
#include <cstdint>
#include <vector>
#include <string>
using namespace std;
using std::cout;
using std::endl;
using std::cin;
using std::vector;

struct packet
{
	string msg;//连接建立、断开标识 
	unsigned int seq;//序列号 
	unsigned int ack;//确认号
	unsigned short len;//数据部分长度
	unsigned short checksum;//校验和
	unsigned short window;//窗口
	char data[1024];//数据长度

	void init_packet()
	{
		this->msg = "waiting";
		this->seq = -1;
		this->ack = -1;
		this->len = -1;
		this->checksum = -1;
		this->window = -1;
		ZeroMemory(this->data, 1024);
	}
};

/*
UDP校验和计算方法：

①按每16位求和得出一个32位的数；
②如果这个32位的数，高16位不为0，则高16位加低16位再得到一个32位的数；
③重复第2步直到高16位为0，将低16位取反，得到校验和。
*/
unsigned short makesum(int count,  char* buf)//校验和
{
	unsigned long sum;
	for (sum = 0; count > 0; count--)
	{
		sum += *buf++;
		sum = (sum >> 16) + (sum & 0xffff);//右移得高16位+高16位清零后的低16位
	}
	return ~sum;
}

void make_pkt(packet* pkt, unsigned int nextseqnum, unsigned int length)
{
	pkt->seq = nextseqnum;
	pkt->len = length;
	pkt->checksum = makesum(sizeof(packet) / 2, pkt->data);
}

// 判断包是否损坏
bool corrupt(packet* pkt)
{
	int count = sizeof(pkt->data) / 2;
	register unsigned long sum = 0;
	unsigned short* buf = (unsigned short*)(pkt->data);
	while (count--) {
		sum += *buf++;
		if (sum & 0xFFFF0000) {
			sum &= 0xFFFF;
			sum++;
		}
	}
	if (pkt->checksum == ~(sum & 0xFFFF))
		return true;
	return false;
}


packet* connecthandler(string msg, int packetnum)
{
	packet* pkt = new packet;
	pkt->msg = msg;
	pkt->len = packetnum;
	return pkt;
}

#define SERVER_IP    "127.0.0.1"  //服务端的IP地址 
#define SERVER_PORT  1062		//服务端的端口号
#define BUFFER sizeof(packet)  //缓冲区大小
#define WINDOWSIZE 20 //滑动窗口的大小（固定）

SOCKADDR_IN addrServer;   //服务端的地址
SOCKADDR_IN addrClient;   //客户端的地址
SOCKET sockServer;//服务端
SOCKET sockClient;//客户端socket

int totalpacket;//数据包个数
int totalack = 0;//正确确认的数据包个数
int curseq = 0;//当前发送的数据包的序列号
int curack = 0;//滑动窗口的最左端，即当前等待被确认的数据包的最小序列号
int dupack = 0;//冗余ack，当累积到3的时候进行快速重传
unsigned long long seqnumber = static_cast<unsigned long long>(UINT32_MAX) + 1;//序列号个数
char buffer[WINDOWSIZE][BUFFER];//选择重传缓冲区
char filepath[100];//文件路径
auto ack = vector<int>(WINDOWSIZE);//维护一个窗口大小的ack数组，作用是进行窗口的滑动操作，确认窗口里的包是否都被ack了
int sendwindow = WINDOWSIZE* BUFFER;//发送窗口缓冲区的初始大小
int recvSize;  //接收到的信息长度
int length = sizeof(SOCKADDR);
int totalLen = 0;
int STATE = 0;
float cwnd=1;//拥塞窗口初始大小
int ssthresh = 32;//阈值，初始大小32

enum state
{
	SLOWSTART,//慢启动
	AVOID,//拥塞避免
	FASTRECO//快速恢复
};



//拥塞避免的上限是滑动窗口大小
int minwindow(int a, int b)
{
	if (a >= b)
		return b;
	else
		return a;
}



//初始化工作
void inithandler()
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;//socket加载时错误提示 
	wVersionRequested = MAKEWORD(2, 2);//版本为2.2 
	err = WSAStartup(wVersionRequested, &wsaData);//加载Socket库的dll
	if (err != 0) {
		cout << "WSAStartup失败了，错误代码是: " << err << endl;//无法找到winsock.dll 
		return;
	}
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		cout << "错误：找不到Winsock.dll的版本" << endl;
		WSACleanup();
	}
	sockServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	//设置socket为非阻塞模式 
	//在阻塞模式下，在I/O操作完成前，执行的操作函数一直等候而不会立即返回，该函数所在的线程会阻塞在这里。
    //相反，在非阻塞模式下，socket函数会立即返回，而不管I/O是否完成，该函数所在的线程会继续运行。
	int iMode = 1; //1：非阻塞，0：阻塞 
	ioctlsocket(sockServer, FIONBIO, (u_long FAR*) & iMode);//非阻塞设置 
	addrServer.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(SERVER_PORT);
	err = bind(sockServer, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
	if (err) {
		err = GetLastError();
		cout << "无法绑定端口" << SERVER_PORT << "，错误码是：" << err << endl;
		WSACleanup();
		return;
	}
	else{
		cout << "***成功创建服务端***" << endl;
	}
	for (int i = 0; i < WINDOWSIZE; i++){
		ack[i] = 1;//初始都标记为1
	}
}

//超时重传
void timeouthandler()
{
	BOOL flag=false;
	packet* pkt1 = new packet;
	pkt1->init_packet();
    //ack数组里该包对应的元素标记为2，证明这个包只被发送了，并没有被确认
    //在ackhandler中，确认的包被置3
	if (ack[curack % WINDOWSIZE] == 2)//快速重传之后还有没被确认的，认为包丢失
	{
		for (int i = curack; i != curseq; i = (i++) % seqnumber)
		{
			memcpy(pkt1, &buffer[i % WINDOWSIZE], BUFFER);
			sendto(sockServer, (char*)pkt1, BUFFER, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
			cout << "***重传第 " << i << " 号数据包***" << endl;
			flag = true;
		}		
	}
}

//快速重传操作
void fasthandler()
{
	packet* pkt1 = new packet;
	pkt1->init_packet();
    //把当前滑动窗口最左端的包（当前等待被确认的数据包的最小序列号）一直到当前发送的序列号的包都重传一遍
    //即把所有窗口里已发送，未确认的包重传一遍
	for (int i = curack; i != curseq; i = (i++) % seqnumber)
	{
		memcpy(pkt1, &buffer[i % WINDOWSIZE], BUFFER);
		sendto(sockServer, (char*)pkt1, BUFFER, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
		cout << "***重传第 " << i << " 号数据包***" << endl;
	}
}

void ackhandler(unsigned int a)
{
	long long index = a;	
	switch (STATE)//状态
	{
	case SLOWSTART://慢启动状态
		if ((index + seqnumber - curack) % seqnumber < minwindow(cwnd, WINDOWSIZE))
			//收到的数据包<拥塞避免上限
		{
			cout << "<<<<<<<<收到了" << index << "号数据包的ack<<<<<<<<" << endl << endl;
			ack[index % WINDOWSIZE] = 3;//已确认
			if (cwnd <= ssthresh)//拥塞窗口小于阈值，使用慢启动
			{
				cwnd++;//每收到一个ack，cwnd+1；指数增加
				cout << "==========================慢启动阶段============================" << endl;
				cout << "cwnd=  " << cwnd << "     sstresh= " << ssthresh << endl << endl;

			}
			else
			{
				STATE = AVOID;//拥塞窗口大于等于阈值，进入拥塞避免阶段
			}
			//累积确认
			for (int j = curack; j != (index + 1) % seqnumber; j = (++j) % seqnumber)
				//滑动窗口最左端到当前序列号
			{
				ack[j % WINDOWSIZE] = 1;//ack初始化
				++totalack;//确认个数++
				curack = (curack + 1) % seqnumber;//滑动窗口最左端++
			}
		}
		else if (index == curack - 1)
			//收到的ack是当前等待被确认的数据包的最小序列号-1的话，就证明收到了冗余ack
		{
			dupack++;
			//冗余ack++
			//快速重传是指如果连续收到3个重复的确认报文(DUPACK)则认为该报文很可能丢失了
			//此时即使重传定时器没有超时，也重传
			if (dupack == 3)//进入快速恢复状态
			{
				fasthandler();//快速重传
				ssthresh = cwnd / 2;//阈值设置为拥塞窗口的一半
				/*
				①将CWND设置为新的ssthresh（减半后的ssthresh）

				②拥塞窗口cwnd值增大，即等于 ssthresh + 3 * MSS 。
				理由：既然发送方收到三个重复的确认，就表明有三个分组已经离开了网络。
				这三个分组不再消耗网络 的资源而是停留在接收方的缓存中。
				可见现在网络中并不是堆积了分组而是减少了三个分组。
				因此可以适当把拥塞窗口扩大了些。
				*/
				cwnd = ssthresh + 3;//扩大拥塞窗口
				STATE = AVOID;//进入拥塞避免阶段
				dupack = 0;//冗余ack归零
			}
		}
		break;
	case AVOID:
		if ((index + seqnumber - curack) % seqnumber < minwindow(cwnd, WINDOWSIZE))
		{
			cout << "<<<<<<<<收到了" << index << "号数据包的ack<<<<<<<<" << endl << endl;
			ack[index % WINDOWSIZE] = 3;//已确认
			cwnd = cwnd + 1 / cwnd;//每接收一个ACK增长1/cwnd
			cout << "==========================达到阈值，进入拥塞避免阶段============================" << endl;
			cout << "cwnd=  " << int(cwnd) << "     sstresh=" << ssthresh << endl << endl;
			//累积确认
			for (int j = curack; j != (index + 1) % seqnumber; j = (++j) % seqnumber)
			{
				ack[j % WINDOWSIZE] = 1;
				++totalack;
				curack = (curack + 1) % seqnumber;
			}
		}
		else if (index == curack - 1)
		{
			dupack++;
			if (dupack == 3)
			{
				fasthandler();//快速重传
				STATE = AVOID;//拥塞避免
				dupack = 0;
			}
		}
		break;		
	}
			
}

int main()
{
	//初始化
	inithandler();
	//读文件
	cout << "请输入发送的文件目录：";
	cin >> filepath;
	ifstream is(filepath, ios::in | ios::binary);//以二进制方式打开文件
	if (!is.is_open()) {
		cout << "错误：无法打开该文件" << endl;
		exit(1);
	}
	is.seekg(0, std::ios_base::end);  //将文件流指针定位到流的末尾
	int length1 = is.tellg(); //记录文件长度bytes
	totalLen = is.tellg();
	totalpacket = length1 / 1024 + 1;
	cout << "文件大小： " << length1 << "Bytes, 数据包个数：" << totalpacket << endl;
	is.seekg(0, std::ios_base::beg);  //将文件流指针重新定位到流的开始

	packet* pkt = new packet;
	/*建立连接（三次握手）
	①客户端发送的msg=request的数据报请求连接
	②服务器收到请求后向客户端发送一个 msg=severOK 的数据报，表示服务器准备好发送数据
	③客户端收到 severOK 后发送 msg=clientOK 的数据报，表示客户端准备好接收数据
	④服务器收到 clientOK 状态码后发送数据了
	*/
	while (true)
	{
		recvSize = recvfrom(sockServer, (char*)pkt, BUFFER, 0, ((SOCKADDR*)&addrClient), &length);
		int count = 0;
		int waitcount = 0;
		while (recvSize < 0)
		{
			count++;
			Sleep(100);
			if (count > 20)
			{
				cout << "***当前没有客户端请求连接***" << endl;
				count = 0;
				break;
			}
		}
		//握手建立连接阶段
		if (pkt->msg =="request")
		{
			clock_t st = clock();//开始计时
			cout << "***开始建立连接***" << endl;
			int stage = 0;
			bool runFlag = true;
			int waitCount = 0;
			while (runFlag)
			{
				switch (stage)
				{
				case 0://发送severOK阶段
					pkt = connecthandler("severOK", totalpacket);
					sendto(sockServer, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
					Sleep(100);
					stage = 1;
					break;
				case 1://等待接收clientOK阶段
					ZeroMemory(pkt, sizeof(*pkt));
					recvSize = recvfrom(sockServer, (char*)pkt, BUFFER, 0, ((SOCKADDR*)&addrClient), &length);
					if (recvSize < 0)
					{
						++waitCount;
						Sleep(200);
						if (waitCount > 20)
						{
							runFlag = false;
							cout << "***建立连接失败***等待重新建立连接***" << endl;
							break;
						}
						continue;
					}
					else
					{
						if (pkt->msg == "clientOK")
						{
							pkt->init_packet();
							cout << "***文件传输开始***" << endl;
							memcpy(pkt->data, filepath, strlen(filepath));
							pkt->len = strlen(filepath);
							sendto(sockServer, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
							stage = 2;					
						}
					}
					break;
					//数据传输
				case 2:
					if (totalack == totalpacket)//数据包传输完毕
					{
						pkt->init_packet();
						pkt->msg = "successfully";
						double endtime = (double)(clock() - st) * 1000.0 / CLOCKS_PER_SEC;
						cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << endl;
						cout << "****文件传输完毕****" << endl;
						cout << "文件传输时间:  " << endtime<< "ms" << endl;
						cout << "吞吐率："<< (double)totalLen * 8*1000/endtime /1000000<< "Mbps" << endl;
						sendto(sockServer, (char*)pkt, sizeof(*pkt), 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
						runFlag = false;
						exit(0);
						break;
					}
					//滑动窗口
					while ((curseq + seqnumber - curack) % seqnumber <WINDOWSIZE && sendwindow>0)
						//只要窗口还没被用完，就持续发送数据包
					{						
						ZeroMemory(buffer[curseq % WINDOWSIZE], BUFFER);
						pkt->init_packet();
						if (length1 >= 1024)
						{
							is.read(pkt->data, 1024);
							make_pkt(pkt, curseq, 1024);

							length1 -= 1024;							
						}
						else
						{
							is.read(pkt->data, length1);
							make_pkt(pkt, curseq, length1);	
						}
						memcpy(buffer[curseq % WINDOWSIZE], pkt, BUFFER);
						//从pkt开始拷贝BUFFER大小到buffer中
						sendto(sockServer, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
						cout << "发送了序列号为 " << curseq << " 的数据包" << endl << endl;
						char buf1[128];//string->char数组
	                    itoa(pkt->checksum,buf1,2);//char数组->2进制
	                    string cks(buf1);//数组->字符串
	                    cout<<"校验和:"<<cks<<endl;//以二进制的形式输出校验和
						ack[curseq % WINDOWSIZE] = 2;//已发送待确认
						++curseq;//发送序列号++
						curseq %= seqnumber;
					}
					//等待接收确认ack
					pkt->init_packet();
					recvSize = recvfrom(sockServer, (char*)pkt, BUFFER, 0, ((SOCKADDR*)&addrClient), &length);
					if (recvSize < 0)
					{  //recvSize<0代表没接收到发送的信息，按照超时重传机制应该在到时间的时候重新发送
						waitcount++;
						Sleep(200);
						if (waitcount > 20)//往返时延
						{
							timeouthandler();
							waitcount = 0;
						}
					}
					else//接收到了ack之后 调用ackhandler来对ack数组中的元素进行累积确认
					{				//ackhandler:滑动窗口机制		
						ackhandler(pkt->ack);
						sendwindow = pkt->window;	
						cout<<"当前发送端窗口大小："<<sendwindow<<endl;
					}
					break;
				}
			}
		}
	}
	//关闭socket 
	closesocket(sockServer);
	WSACleanup();
	return 0;
}
