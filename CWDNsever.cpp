#pragma warning(disable:4996)
#pragma comment(lib,"ws2_32.lib") 
#include <stdlib.h> 
#include <time.h> 
#include <WinSock2.h> 
#include <fstream> 
#include <iostream>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include<string>
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

//计算校验和
unsigned short makesum(int count, char* buf)
{
	unsigned long sum;
	for (sum = 0; count > 0; count--)
	{
		sum += *buf++;
		sum = (sum >> 16) + (sum & 0xffff);
	}
	return ~sum;
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
	if (pkt->checksum == ~(sum & 0xFFFF)){
		char buf1[128];
	itoa(pkt->checksum+sum,buf1,2);
	string cks(buf1);
	cout<<"校验和:"<<cks<<endl;//以二进制的形式输出校验和
		return true;}
	char buf1[128];
	itoa(pkt->checksum+sum,buf1,2);
	string cks(buf1);
	cout<<"校验和:"<<cks<<endl;//以二进制的形式输出校验和
	return false;
}

void make_mypkt(packet* pkt, long long ack, unsigned short window)
{
	pkt->ack = ack;
	pkt->window = window;
}


packet* connecthandler(string msg)
{
	packet* pkt = new packet;
	pkt->init_packet();
	pkt->msg = msg;
	return pkt;
}


#define SERVER_PORT  1062 //接收数据的端口号 
#define SERVER_IP    "127.0.0.1" //  服务端的 IP 地址 
#define BUFFER sizeof(packet)
#define WINDOWSIZE 20
SOCKET socketClient;//客户端socket
SOCKADDR_IN addrServer; //服务端地址


char filename[100];
int waitseq = 0;//等待的数据包
int totalpacket;//数据包总数
int seqnum = 40;//序列号个数
int len = sizeof(SOCKADDR);
int totalrecv=0;//总共接收的数据包的个数
int recvwindow = WINDOWSIZE*BUFFER;//接收窗口缓冲区大小
unsigned long long seqnumber = static_cast<unsigned long long>(UINT32_MAX) + 1;//总共的序列号个数

//模拟丢包
BOOL lossInLossRatio(float lossRatio) {
	int lossBound = (int)(lossRatio * 100);//丢包率的百分数
	int r = rand() % 101;//产生0~100之间的随机整数
	if (r <= lossBound) {//小于丢包率
		return TRUE;//丢包
	}
	return FALSE;//没丢
}

//初始化
void init()
{
	WORD wVersionRequested;//加载socket库 
	WSADATA wsaData; 
	int err;//socket加载时错误提示
	wVersionRequested = MAKEWORD(2, 2);//版本 2.2 
	err = WSAStartup(wVersionRequested, &wsaData);//加载Socket库的dll
	if (err != 0)//找不到 winsock.dll 
	{
		cout<<"WSAStartup failed with error: "<<err<<endl;
		return ;
	}
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		cout<<"Could not find a usable version of Winsock.dll"<<endl;
		WSACleanup();
	}
	else
	{
		cout<<"socket创建成功"<<endl;
	}
	socketClient = socket(AF_INET, SOCK_DGRAM, 0);
	addrServer.sin_addr.S_un.S_addr = inet_addr(SERVER_IP);
	addrServer.sin_family = AF_INET;
	addrServer.sin_port = htons(SERVER_PORT);

}

int main()
{
	//初始化
	init();
	std::ofstream out_result;
	packet *pkt=new packet;
	pkt->init_packet();
	int stage = 0;		
	float packetLossRatio = 0.2;  //默认包丢失率 0.2				  
	srand((unsigned)time(NULL));//随机种子，放在循环的最外面 
	BOOL b;
	while (true)
	{
	//建立连接
		pkt->init_packet();
		pkt->msg = "request";//首先发送"request"，等待server回复
		sendto(socketClient, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
		while (true){
			switch (stage)
			{
			case 0://等待握手
				recvfrom(socketClient, (char*)pkt, sizeof(*pkt), 0, (SOCKADDR*)&addrServer, &len);
				totalpacket = pkt->len;
				cout << "~~~~准备建立连接~~~~数据包个数：" << totalpacket << endl;
				pkt->init_packet();
				pkt=connecthandler("clientOK");
				sendto(socketClient, (char*)pkt,sizeof(*pkt) , 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
				stage = 1;
				break;
			case 1:
				recvfrom(socketClient, (char*)pkt, sizeof(*pkt), 0, (SOCKADDR*)&addrServer, &len);
				memcpy(filename, pkt->data, pkt->len);
				char ul[5];
				ul[0]='1';
				for(int i=pkt->len-4,j=1;i<pkt->len;i++,j++){
					ul[j]=filename[i];
				}
				for(int i=pkt->len-4,j=0;i<pkt->len+1;i++,j++){
					filename[i]=ul[j];
				}
				
				out_result.open(filename, ios::out | ios::binary);
				cout << "文件路径：" << filename << endl;
				if (!out_result.is_open())
				{
					cout << "文件打开失败" << endl;
					exit(1);
				}
				stage = 2;
				break;
	//文件传输
			case 2:
				pkt->init_packet();
				recvfrom(socketClient, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrServer, &len);
				if (pkt->msg == "successfully")
				{
					cout << "**************************************" << endl;
					cout << "***文件传输完毕***";
					goto success;

				}
                //GBN			
				if (pkt->seq == waitseq && totalrecv < totalpacket&&!corrupt(pkt))
				{
					b = lossInLossRatio(packetLossRatio);
					if (b) {
						cout << "***************第  " << pkt->seq << " 号数据包丢失" << endl << endl;
						continue;
					}
					cout << "***收到第" << pkt->seq << "号数据包***" << endl << endl;
					recvwindow -= BUFFER;//接收后窗口大小-BUFFER
					cout<<"当前接收端窗口大小："<<recvwindow<<endl;
					out_result.write(pkt->data, pkt->len);//把pkt所指的内存写入len个字节到所指的文件内
					out_result.flush();//清空缓存区
					recvwindow += BUFFER;//应用进程读取后，窗口大小+BUFFER
					cout<<"写入后,当前接收端窗口大小："<<recvwindow<<endl;
					make_mypkt(pkt, waitseq, recvwindow);	//ack=current waitseq-1	,recvwindow+=BUFFER
					cout << "----------发送对第" << waitseq << "号数据包的确认----------" << endl;
					sendto(socketClient, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
					waitseq++;
					waitseq %= seqnumber;
					totalrecv++;
				}
				else
				{
					make_mypkt(pkt, waitseq - 1, recvwindow);
					cout << "-------不是期待的数据包，发送了一个重复ack--------" << waitseq - 1 << endl;
					cout<<"当前接收端窗口大小："<<recvwindow<<endl;
					sendto(socketClient, (char*)pkt, BUFFER, 0, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
				}
				
			}
		}
		
	success:
		{
			out_result.close();
			exit(0);
		}
	}
	closesocket(socketClient);
	WSACleanup();
	return 0;
}
