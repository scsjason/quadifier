#include <iostream>
#include <vector>
#include <ctime>
#include <conio.h>
#include <math.h>
#include <winsock2.h>
#include "vrpn_Analog.h"
#include "vrpn_Tracker.h"

using namespace std;

//-----------------------------------------------------------------------------
//
// Copyright (C) 2013-14 James Ward, University of Hull
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
//    1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software.
//
//    2. If you use this software in a product, an acknowledgment in the
//    product documentation is required.
//
//    3. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
//    4. This notice may not be removed or altered from any source
//    distribution.
//
//-----------------------------------------------------------------------------

/// VRPN "bridge" which connects to a VRPN server and serves tracking data
/// in a simple binary format so that it can easily be read by a fairly
/// dumb Unity script. This is a proof of concept, and would benefit from
/// a rewrite using Boost ASIO or similar instead of Winsock.
/// Tested and working using Vicon Tracker and Razer Hydra
/// J.Ward 12/01/13

//-----------------------------------------------------------------------------

#pragma pack (push, 1)
/// defines the format of the tracker data sent to Unity
struct TrackerData {
    float timeStamp;    ///< time in seconds
    int   sensor;       ///< sensor number
    float position[3];  ///< position vector
    float rotation[4];  ///< orientation quaternion

    TrackerData() :
        timeStamp(0.0f),
        sensor(0)
    {
    }

    /// fill in the structure, given VRPN tracking data
    void set( const vrpn_TRACKERCB & tracker ) {
        // time in seconds
        timeStamp = (float)(tracker.msg_time.tv_sec) +
                    static_cast<float>(tracker.msg_time.tv_usec) * 1.0e-6f;

        // sensor number
        sensor = tracker.sensor;

        // position
        position[0] = static_cast<float>( tracker.pos[0] );
        position[1] = static_cast<float>( tracker.pos[1] );
        position[2] = static_cast<float>( tracker.pos[2] );

        // calculate scale factor to normalise the quaternion
        double sum = 0.0;
        for (unsigned i=0; i<4; i++)
            sum += tracker.quat[i] * tracker.quat[i];
        double scale = 1.0 / sqrt( sum );

        // orientation quaternion (normalise before sending to Unity)
        rotation[0] = static_cast<float>( tracker.quat[0] * scale );
        rotation[1] = static_cast<float>( tracker.quat[1] * scale );
        rotation[2] = static_cast<float>( tracker.quat[2] * scale );
        rotation[3] = static_cast<float>( tracker.quat[3] * scale );
    }

	/// fill in the structure, given VRPN tracking data
	void set(float pX, float pY, float pZ, float q0, float q1, float q2, float q3, unsigned long tStamp, int sensorID) {
		// time in seconds
		timeStamp = (float)tStamp;

		// sensor number
		sensor = sensorID;

		// position
		position[0] = pX;
		position[1] = pY;
		position[2] = pZ;

		// calculate scale factor to normalise the quaternion
		double sum = 0.0;
		float quat[4] = { q0, q1, q2, q3 };
		for (unsigned i = 0; i<4; i++)
			sum += quat[i] * quat[i];
		double scale = 1.0 / sqrt(sum);

		// orientation quaternion (normalise before sending to Unity)
		rotation[0] = static_cast<float>(quat[0] * scale);
		rotation[1] = static_cast<float>(quat[1] * scale);
		rotation[2] = static_cast<float>(quat[2] * scale);
		rotation[3] = static_cast<float>(quat[3] * scale);
	}
};
#pragma pack (pop)

//-----------------------------------------------------------------------------

const short SERVER_PORT = 3010;

//-----------------------------------------------------------------------------

class Server {
public:
    /// default constructor
    Server();

    /// destructor
    virtual ~Server();

    /// the server thread
    static unsigned __stdcall serverThread( void *userData );

    /// start the server
    bool start();

    /// stop the server
    void stop();

    /// returns true if the server is running
    bool isRunning() const;

    /// send tracking data to the client
    bool send( const TrackerData & data );

private:
    /// send data
    unsigned send( SOCKET sock, const void *data, unsigned size );
    
    /// open Winsock library
    bool openWinsock();

    /// close Winsock library
    void closeWinsock();

private:
    bool    m_running;  ///< is the server running?
    HANDLE  m_thread;   ///< server thread handle
    SOCKET  m_socket;   ///< socket connection
    bool    m_quit;     ///< quit flag
};

//-----------------------------------------------------------------------------

Server::Server() {
    m_running = false;
    m_thread = 0;
    m_socket = 0;
    m_quit = false;
    openWinsock();
}

//-----------------------------------------------------------------------------

Server::~Server() {
    stop();
    closeWinsock();
}

//-----------------------------------------------------------------------------

unsigned Server::serverThread( void *userData ) {
    Server *server = reinterpret_cast<Server*>( userData );

    // create socket
    SOCKET sock = socket( PF_INET, SOCK_STREAM, 0 );
    if ( sock == INVALID_SOCKET ) {
        cerr << "Server: failed to create socket\n";
        _endthreadex( 0 );
        return 0;
    }

    // bind socket
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons( SERVER_PORT );
    address.sin_addr.s_addr = htonl( INADDR_ANY );
    if (
        bind( sock, (const struct sockaddr*)&address, sizeof(address) )
        == SOCKET_ERROR
    ) {
        cerr << "Server: failed to bind socket\n";
        closesocket( sock );
        _endthreadex(0);
        return 0;
    }

    do {
        cout << "Server: listening\n";

        // listen for connection
        if ( listen( sock, SOMAXCONN ) == SOCKET_ERROR ) {
            cerr << "Server: listen failed\n";
            break;
        }

        int numRead = 0;
        do {
            // create an fd_set containing one socket (sock)
            fd_set set;
            FD_ZERO( &set );
            FD_SET( sock, &set );

            // check socket status, with 250ms timeout period
            struct timeval timeout = { 0, 250*1000 };
            numRead = select( 1, &set, 0, 0, &timeout );
        } while ( (numRead == 0) && !server->m_quit );

        if ( server->m_quit ) break;

        // accept connection
        struct sockaddr_in client;
        int size = sizeof( client );
        SOCKET link;
        do {
            // the use of select() above should mean that we are
            // ready to accept() without blocking by this stage
            link = accept(sock, (struct sockaddr*)&client, &size);
        } while (link == INVALID_SOCKET);

        // successful?
        if ( link == INVALID_SOCKET ) {
            cerr << "Server: accept failed\n";
            break;
        }

        server->m_socket = link;

        cout << "connection\n";

        vector<char> buffer(256);

        int result = 0;
        do {
            result = recv( link, &buffer[0], buffer.size(), 0);
            if ( result > 0 )
                cout << "Server: received " << result << " bytes\n";
            else if ( result == 0 )
                cout << "Server: closing connection\n";
            else {
                cerr << "Server: recv failed with error " << WSAGetLastError() << endl;
                cerr << "Closing connection (assume client disconnected)\n";
                break;
            }
        } while (result > 0);

        // close the socket
        shutdown( server->m_socket, SD_SEND );
        closesocket( server->m_socket );
        server->m_socket = 0;
    } while ( !server->m_quit );

    // close listen socket
    closesocket( sock );
    sock = 0;

    // end the thread
    _endthreadex(1);

    return 1;
}

//-----------------------------------------------------------------------------

bool Server::start() {
    // is server already running?
    if ( isRunning() ) return true;

    m_quit = false;

    // start server thread
    unsigned int threadId = 0;
    m_thread = reinterpret_cast<HANDLE>(_beginthreadex(
        0,              // no security attributes (child can't inherit)
        1024*1024,      // 1MB stack size
        serverThread,   // thread function
        this,           // pointer to parent
        0,              // run immediately
        &threadId       // out: returns thread ID
    ));

    // check result
    if ( m_thread != 0 ) {
        // check whether it started
        if ( WaitForSingleObject( m_thread, 100L) == WAIT_OBJECT_0 ) {
            m_running = false;
            return false;
        } else
            m_running = true;

        return true;
    } else {
        // failed to start
        m_running = false;

        return false;
    }
}

//-----------------------------------------------------------------------------

void Server::stop() {
    // is server already stopped?
    if ( !isRunning() ) return;

    // set flag to request thread to stop
    m_quit = true;

    // wait for it to stop (give it 4 seconds)
    if ( WaitForSingleObject(m_thread, 4000L) == WAIT_OBJECT_0 ) {
        // thread has exited
        CloseHandle( m_thread );
    } else {
        // timeout: thread still running
    }

    // clear thread handle
    m_thread = 0;

    // reset running state and stop request flag
    m_running = false;
    m_quit = false;
}

//-----------------------------------------------------------------------------

bool Server::isRunning() const {
    return m_running;
}

//-----------------------------------------------------------------------------

bool Server::send( const TrackerData & data ) {
    if ( m_socket != 0 )
        return send( m_socket, &data, sizeof(data) ) == sizeof(data);
    else
        return false;
}

//-----------------------------------------------------------------------------

unsigned Server::send( SOCKET sock, const void *data, unsigned size ) {
    int numRemain = static_cast<int>(size);

    const char *ptr = reinterpret_cast<const char *>(data);

    do {
        // send a packet
        int numSent = ::send( sock, ptr, numRemain, 0 );
        if ( numSent == SOCKET_ERROR ) break;

        // update buffer pointer and byte count
        ptr += numSent;
        numRemain -= numSent;
    } while ( numRemain > 0 );

    // return number of bytes sent
    return size - numRemain;
}

//-----------------------------------------------------------------------------

bool Server::openWinsock() {
    WORD version = MAKEWORD(2,2);
    WSADATA wsa;
    if ( WSAStartup( version, &wsa ) != 0 )
        return false;

    if ( wsa.wVersion == version )
        return true;
    else {
        WSACleanup();
        return false;
    }
}

//-----------------------------------------------------------------------------

void Server::closeWinsock() {
    WSACleanup();
}

//-----------------------------------------------------------------------------

unsigned frames = 0;
unsigned long tStamp = -1;
bool debugOutput = false;
float gpX = 0, gpY = 0, gpZ = 0, gq0 = 0, gq1 = 0, gq2 = 0, gq3 = 1;

void VRPN_CALLBACK handleTracker( void *userData, const vrpn_TRACKERCB tracker ) {
    Server *server = reinterpret_cast<Server*>( userData );

	static int first = 1;

    if (tracker.sensor == 0) ++frames;

if (first || debugOutput) {
	cout << "Seen tracked object:\n";
	cout << "Tracker " << tracker.sensor << ": "
		<< tracker.pos[0] << ',' << tracker.pos[1] << ',' << tracker.pos[2] << ','
		<< tracker.quat[0] << ',' << tracker.quat[1] << ',' << tracker.quat[2] << ','
		<< tracker.quat[3]
		<< endl;
	first = 0;
}

// Set global data
gpX = tracker.pos[0];
gpY = tracker.pos[1];
gpZ = tracker.pos[2];
gq0 = tracker.quat[0];
gq1 = tracker.quat[1];
gq2 = tracker.quat[2];
gq3 = tracker.quat[3];

// tracker data to send to Unity client
TrackerData data;
data.set(tracker);

// send the data
server->send(data);

struct timeval currentTime;
vrpn_gettimeofday(&currentTime, NULL);
tStamp = currentTime.tv_usec + (1000000L * currentTime.tv_sec);
}

// String equality test (case insensitive)
// PORT: not all platforms have stricmp(), 
#define strieq(a,b) (stricmp(a,b)==0)

vrpn_Tracker_Remote *tracker;

// To handle ctrl-C from console to kill vrpnbridge
BOOL WINAPI consoleHandler(DWORD signal) {

	if (signal == CTRL_C_EVENT)
		printf("Ctrl-C caught\n"); // do cleanup

	// Bit harsh . . .
	delete tracker;

	exit(0);

	return TRUE;
}

int main(int Argc, char **Argv)
{

	const char *_tObj = NULL;
	bool errFlag = false;
	unsigned long tLimit = 10000;

	// Good values for the cube
	/*
	gpX = -0.105992;
	gpY = 1.40699;
	gpZ = 1.48177;
	gq0 = -0.104662;
	gq1 = -0.108086;
	gq2 = 0.0404521;
	gq3 = 0.987789;
	*/

	// Try to catch CtrlC in the console
	if (!SetConsoleCtrlHandler(consoleHandler, TRUE)) {
		cerr << "\nERROR: Could not set control handler";
		errFlag = true;
	}

	if (Argc <= 1) {
		cerr << "Error: No args. Need at least -object <objectname>\n";
		errFlag = true;
	}

	// process command line arguments
	for (int n = 1; n < Argc; n++) {
		// get nth argument
		const char *arg = Argv[n];

		// check for our options
		if (strieq(arg, "-obj") || strieq(arg, "-object")) {
			if (n >= (Argc - 1)) {
				cerr << "Error: -object arg needs a name of an object to track, e.g Glasses01@HIVE-CO31958\n";
				errFlag = true;
			}
			else {
				_tObj = Argv[++n];
			}
		}
		else if (strieq(arg, "-debug")) {
			debugOutput = true;
		}
		else if (strieq(arg, "-tLimit")) {
			if (n >= (Argc - 1)) {
				cerr << "Error: -tLimit arg needs an integer value\n";
				errFlag = true;
			}
			else {
				tLimit = atoi(Argv[++n]);
			}
		}
		else if (strieq(arg, "-initLoc")) {
			if (n >= (Argc - 7)) {
				cerr << "Error: -initLoc arg needs 7 float values\n";
				errFlag = true;
			}
			else {
				gpX = atof(Argv[++n]);
				gpY = atof(Argv[++n]);
				gpZ = atof(Argv[++n]);
				gq0 = atof(Argv[++n]);
				gq1 = atof(Argv[++n]);
				gq2 = atof(Argv[++n]);
				gq3 = atof(Argv[++n]);
			}
		}
		else if (strieq(arg, "-h") || strieq(arg, "-help")) {
			cout << "Input args: \n";
			cout << "-object <objectname> where objectname is of the form \n";
			cout << "        object@hostname, e.g. Glasses01@HIVE-CO31958\n";
			cout << "[-debug] turns on debugging output.\n";
			cout << "[-tLimit <timelimit>] where timelimit is an integer value\n";
			cout << "        in microseconds of how long to wait until \"fake\" \n";
			cout << "        tracker data is sent. The fake data is the initial \n";
			cout << "        location specified with -initLoc, or the last seen \n";
			cout << "        location.\n";
			cout << "[-initLoc pX pY pZ q0 q1 q2 q3] this is the initial location \n";
			cout << "        reported for the object until it is first reported by\n";
			cout << "        the tracking system.\n";
			exit(0);
		}
		else {
			cerr << "Error: Unknown arg " << arg << "\n";
			errFlag = true;
		}
	}

	// Catch any errors
	if (errFlag) {
		cerr << "Use -help to list accepted arguments\n";
		cerr << "Press any key to exit!\n";
		cin.get();
		exit(0);
	}

	Server server;
	server.start();

	tracker = new vrpn_Tracker_Remote(_tObj);

	tracker->register_change_handler(&server, handleTracker);

	// record start time
	float t = (float)clock() / CLOCKS_PER_SEC;

	struct timeval currentTime;
	unsigned long tStamp2;
	

	while (!_kbhit()) {
		tracker->mainloop();

		vrpn_gettimeofday(&currentTime, NULL);
		tStamp2 = currentTime.tv_usec + (1000000L * currentTime.tv_sec);
		if ((tStamp2 - tStamp) > tLimit) {
			tStamp = tStamp2;
			if (debugOutput)
				cout << "Faking data: " << gpX << "," << gpY << "," << gpZ << "," << gq0 << "," << gq1 << "," << gq2 << "," << gq3 << "\n";
			// tracker data to send to Unity client
			TrackerData data;
			data.set(gpX,gpY,gpZ,gq0,gq1,gq2,gq3, tStamp2, 0);
			// send the data
			server.send(data);
		}	
	}

    server.stop();

    tracker->unregister_change_handler( &server, handleTracker );
	delete tracker;

    // calculate update rate achieved (for Razer Hydra, I get 250Hz)
    t = (float)clock()/CLOCKS_PER_SEC - t;
    printf("%.2f Hz\n", (float)frames / t);

    return 0;
}

//-----------------------------------------------------------------------------
