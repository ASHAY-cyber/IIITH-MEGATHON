# Collaborative Text Editor

A real-time collaborative text editor built in C with WebSocket support for the hackathon competition. This project enables multiple users to simultaneously edit text files with live cursor tracking and user presence indicators.

## Features

- **Real-time Collaboration**: Multiple users can edit the same document simultaneously  
- **Live Cursor Tracking**: See other users' cursors with unique colors in real-time  
- **File Management**: Create, open, save, and delete files through web interface  
- **WebSocket Communication**: Fast, bidirectional communication for instant updates  
- **Multi-threaded Server**: Handles multiple clients concurrently using pthreads  
- **Cross-platform Access**: Access from any device with a web browser  
- **User Presence**: See who's online and what file they're editing  

## Technical Architecture

### Backend (C Server)
- **HTTP Server** (Port 8080): Serves the web interface and handles file operations  
- **WebSocket Server** (Port 8081): Manages real-time communication between clients  
- **Multi-threading**: Uses pthreads for concurrent client handling  
- **File System**: Stores documents in `./files/` directory  

### Frontend (HTML/JavaScript)
- Simple HTML interface with textarea editor  
- WebSocket client for real-time communication  
- JavaScript handles user interactions and live updates  

## Project Structure

```
├── collab_editor2.c    # Main C server implementation
├── index.html          # Web interface
└── files/              # Directory for stored documents (auto-created)
```

## Dependencies

### Required Libraries
- **OpenSSL**: For WebSocket handshake (SHA1 hashing)  
- **pthreads**: For multi-threading support  
- **Standard C libraries**: stdio, stdlib, string, unistd, sys/socket, etc.  

### Installation (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install build-essential libssl-dev
```

### Installation (CentOS/RHEL)
```bash
sudo yum install gcc openssl-devel
```

## How to Run

### Step 1: Compile the Server
```bash
gcc -o collab_editor collab_editor2.c -lpthread -lssl -lcrypto
```

### Step 2: Run the Server
```bash
./collab_editor
```

### Step 3: Access the Editor
- Open your web browser  
- Navigate to `http://localhost:8080`  
- For network access from other devices: `http://[YOUR_IP_ADDRESS]:8080`  

### Step 4: Start Collaborating
- Multiple users can access the same URL  
- Create or open files through the web interface  
- See real-time edits and cursor movements from other users  

## How It Works

### Client Connection Flow
1. User opens the web interface  
2. JavaScript establishes WebSocket connection to port 8081  
3. Server assigns unique color and username to the client  
4. Real-time synchronization begins  

### Real-time Synchronization
- **Content Changes**: Broadcasted to all connected clients instantly  
- **Cursor Movements**: Tracked and shared with position and color  
- **User Events**: Join/leave notifications sent to all participants  
- **File Operations**: CRUD operations synchronized across clients  

### Key Components

#### Client Management
```c
typedef struct Client {
    int socket;
    char username[64];
    char current_file[256];
    int cursor_pos;
    char color[16];
    int active;
    pthread_mutex_t lock;
    struct Client* next;
} Client;
```

#### WebSocket Protocol
- Implements RFC 6455 WebSocket standard  
- Custom frame parsing and construction  
- Base64 encoding for handshake  
- Message broadcasting system  

#### HTTP API Endpoints
- `GET /` - Serves the main editor interface  
- `GET /api/files` - Lists available files  
- `GET /api/file?name=<filename>` - Retrieves file content  
- `POST /api/file` - Saves file content  
- `DELETE /api/file?name=<filename>` - Deletes file  

## Hackathon Highlights

### Innovation
- **Pure C Implementation**: No high-level frameworks, built from scratch  
- **Efficient WebSocket**: Custom implementation without external libraries  
- **Thread-safe Design**: Proper mutex handling for concurrent access  
- **Cross-platform**: Works on Linux, macOS, and Windows (with minor modifications)  

### Performance
- **Low Latency**: Direct socket communication  
- **Memory Efficient**: Minimal overhead, optimized data structures  
- **Scalable**: Supports up to 50 concurrent users (configurable)  

### User Experience
- **Instant Feedback**: Sub-100ms response time for edits  
- **Visual Indicators**: Color-coded cursors for each user  
- **Simple Interface**: Clean, distraction-free editing environment  

## Security Considerations

- **Input Validation**: Sanitizes file names and content  
- **Buffer Overflow Protection**: Bounded string operations  
- **Resource Limits**: Maximum client connections and buffer sizes  
- **File System Isolation**: Restricts access to designated directory  

## Future Enhancements

- **Syntax Highlighting**: Language-specific code coloring  
- **User Authentication**: Login system with permissions  
- **File Versioning**: Track document history and changes  
- **Rich Text Support**: Bold, italic, and formatting options  
- **Chat Integration**: Built-in communication between collaborators  
- **Mobile Optimization**: Responsive design for tablets and phones  

## Hackathon Achievement

This project demonstrates:
- **System Programming Skills**: Low-level network programming in C  
- **Real-time Systems**: WebSocket implementation and message handling  
- **Concurrent Programming**: Thread management and synchronization  
- **Web Integration**: HTTP server and client-side JavaScript  
- **Problem Solving**: Building complex functionality with minimal dependencies  

## Troubleshooting

### Common Issues

**Port Already in Use**
```bash
sudo lsof -i :8080
sudo kill -9 <PID>
```

**SSL Library Not Found**
```bash
sudo ldconfig
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

**Permission Denied (Files Directory)**
```bash
chmod 755 ./files/
```

## Contributing

1. Fork the repository  
2. Create feature branch (`git checkout -b feature/amazing-feature`)  
3. Commit changes (`git commit -m 'Add amazing feature'`)  
4. Push to branch (`git push origin feature-amazing-feature`)
5. 5. Open Pull Request  

## DRIVE LINK FOR VIDEO 
https://drive.google.com/file/d/1tRapSh2oT6Zkw1CfBu-s3q8-rvcmwQYi/view?usp=drivesdk
## DRIVE LINK FOR PRESENTATION PDF
https://drive.google.com/file/d/1X2c46D1NQSYxMGJBBNhbOdg6zmb0vZUs/view?usp=sharing
## License

This project is open source and available under the MIT License.
