// Imports
import puppeteer from 'puppeteer';
import express from 'express';
import http from 'http';
import { WebSocketServer } from 'ws';

// create express server
const app = express();
const PORT = 3000;
const BOUNDARY = 'myboundary'; // mjpeg setting to separate each frame

let clients = [];
let currentFrameBuffer = null; //holds newest jpeg frame from chrome
let isFrameDirty = false; // Tracks if a new frame is waiting
let page = null;  //browser object
let cdpClient = null;   //chrom dev tool protocl connection( screen casting and touch evets)

//starts chrome headless and enables casting
async function startBrowser() {
    console.log("Launching Puppeteer...");
    const browser = await puppeteer.launch({ 
        protocolTimeout: 30000,
        headless: "new",
        args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage']
    });
    
    page = await browser.newPage();
    await page.setViewport({ width: 800, height: 480, hasTouch: true });
    
    try {
        /* DEMO widget website
        await page.goto('https://demo.home-assistant.io/#/lovelace/0', { 
            waitUntil: 'networkidle2',
            timeout: 30000
        });//*/
        /* UNCLE SAMS CRAWLEY DEMO epos
        await page.goto('https://portal.konnected-technology.com:10086/device/5044ce3505141eb6/48746', { 
            waitUntil: 'networkidle2',
            timeout: 30000
        });*/
         /* Eptura DEMO */
        await page.goto('http://192.168.2.1:10002/device/gizmo/9405', { 
            waitUntil: 'networkidle2',
            timeout: 30000
        });


        console.log("Page loaded successfully");
    } catch (error) {
        console.error("Failed to load page:", error.message);
    }

    //create cdp session
    cdpClient = await page.target().createCDPSession();
    //make cdp enable touch screen
    await cdpClient.send("Emulation.setTouchEmulationEnabled", {
        enabled: true,
        maxTouchPoints: 1
    });

    //capture frames from chrome
    cdpClient.on('Page.screencastFrame', async (frameObject) => {
        const { data, sessionId } = frameObject;
        
        try {
           cdpClient.send('Page.screencastFrameAck', {sessionId}).catch(()=>{});
        } catch (error) {}

        currentFrameBuffer = Buffer.from(data, 'base64');
        isFrameDirty = true; // Flag that a new frame is ready
    });

    //start cdp screen casting 
    await cdpClient.send('Page.startScreencast', {
        format: 'jpeg',
        quality: 35, 
        maxWidth: 800,
        maxHeight: 480,
        everyNthFrame: 4,
    });
    
    console.log("CDP Screencast Active - Touch-enabled streaming at 800x480");
}

// buffer - sends mjpeg to clients: 
// Broadcasts at 10- 20fps FPS 
// sends only if the screen actually changed
setInterval(() => {
    if (!currentFrameBuffer || clients.length === 0 || !isFrameDirty) return;
    isFrameDirty = false; 
    const deadClients = [];

    clients.forEach((res) => {
        try {
            res.write(`--${BOUNDARY}\r\n`);
            res.write('Content-Type: image/jpeg\r\n');
            res.write(`Content-Length: ${currentFrameBuffer.length}\r\n\r\n`);
            res.write(currentFrameBuffer);
            res.write('\r\n');
        } catch (err) {
            deadClients.push(res);
        }
    });
    
    //remove dc clients
    if (deadClients.length > 0) {
        clients = clients.filter(c => !deadClients.includes(c));
    }
}, 50); //100 = 10fps, 50 =20fps

// Touch event handler
let isSimulating = false;
let pendingTouch = null; 
//converts received websocket commands to chrome touches
async function simulateTouch(x, y, type) {
    if (!cdpClient) return;

    if (isSimulating) {
        pendingTouch = { x, y, type };
        return; 
    }

    isSimulating = true;

    try {
        if (type === 'touchend') {
            console.log("touch end");
            await cdpClient.send('Input.dispatchTouchEvent', {
                type: 'touchEnd',
                touchPoints: [] 
            });
        } else {
            const cdpType = type === 'touchstart' ? 'touchStart' : 'touchMove';
            console.log(cdpType, "touch ");
            await cdpClient.send('Input.dispatchTouchEvent', {
                type: cdpType,
                touchPoints: [{ x: Math.round(x), y: Math.round(y), id: 1 }] 
            });
        }
    } catch (error) {
        console.error('[CDP]  Touch error:', error.message);
    } finally {
        isSimulating = false;
        //process newest queued touch
        if (pendingTouch) {
            const next = pendingTouch;
            pendingTouch = null;
            simulateTouch(next.x, next.y, next.type);
        }
    }
}

// HTTP / WEBSOCKET SERVER
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/touch' });

wss.on('connection', (ws) => {
    console.log(' Touch WebSocket client connected');
    ws.on('message', (data) => {
        try {
            const touchEvent = JSON.parse(data);
            simulateTouch(touchEvent.x || 0, touchEvent.y || 0, touchEvent.type);
        } catch (error) {}
    });
    ws.on('close', () => console.log('Touch WebSocket client disconnected'));
});

//mjpeg streaming endpoint
app.get('/stream', (req, res) => {
    const clientIP = req.ip || req.socket.remoteAddress;
    console.log(` Stream client connected: ${clientIP}`);
    
    res.writeHead(200, {
        'Content-Type': `multipart/x-mixed-replace; boundary=${BOUNDARY}`,
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Connection': 'keep-alive',
        'Pragma': 'no-cache',
        'Expires': '0'
    });

    if (currentFrameBuffer) {
        try {
            res.write(`--${BOUNDARY}\r\n`);
            res.write('Content-Type: image/jpeg\r\n');
            res.write(`Content-Length: ${currentFrameBuffer.length}\r\n\r\n`);
            res.write(currentFrameBuffer);
            res.write('\r\n');
        } catch (err) {}
    }

    clients.push(res);
    
    req.on('close', () => {
        console.log(` Stream client disconnected: ${clientIP}`);
        clients = clients.filter(c => c !== res);
    });
});

server.listen(PORT, '0.0.0.0', async () => {
    console.log(`Server running on http://0.0.0.0:${PORT}`);
    await startBrowser();
});

//shutdown of server
process.on('SIGINT', () => {
    console.log('\n Shutting down...');
    clients.forEach(res => res.end());
    wss.close();
    process.exit(0);
});
