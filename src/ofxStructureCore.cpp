#include "ofxStructureCore.h"

ofxStructureCore::ofxStructureCore()
{
	_captureSession.setDelegate( this );
}

bool ofxStructureCore::setup( const Settings& settings )
{
	if ( _captureSession.startMonitoring( settings ) ) {
		ofLogNotice( ofx_module() ) << "Sensor " << serial() << " initialized.";
	} else {
		ofLogError( ofx_module() ) << "Sensor " << serial() << " failed to initialize.";
	}
}

bool ofxStructureCore::start()
{
	_isStreaming = _captureSession.startStreaming();

	if ( !_isStreaming && !_streamOnReady ) {
		ofLogWarning( ofx_module() ) << "Sensor " << serial() << " didn't start, will retry on Ready signal (call stop() to cancel)...";
		_streamOnReady = true;  // stream when we get the ready signal
	}
	return _isStreaming;
}

void ofxStructureCore::stop()
{
	_captureSession.stopStreaming();
	_isStreaming   = false;
	_streamOnReady = false;
}

void ofxStructureCore::update()
{
	_isFrameNew = _depthDirty || _irDirty || _visibleDirty;
	// todo: threadsafe access to internal frame data
	if ( _depthDirty ) {
		{
			std::unique_lock<std::mutex> lck( _frameLock );
			depthImg.getPixels().setFromPixels( _depthFrame.depthInMillimeters(), _depthFrame.width(), _depthFrame.height(), 1 );
		}
		depthImg.update();
		// update point cloud
		updatePointCloud();
		_depthDirty = false;
	}
	if ( _irDirty ) {
		{
			std::unique_lock<std::mutex> lck( _frameLock );
			irImg.getPixels().setFromPixels( _irFrame.data(), _irFrame.width(), _irFrame.height(), 1 );
		}
		irImg.update();
		_irDirty = false;
	}
	if ( _visibleDirty ) {
		{
			std::unique_lock<std::mutex> lck( _frameLock );
			visibleImg.getPixels().setFromPixels( _visibleFrame.rgbData(), _visibleFrame.width(), _visibleFrame.height(), 3 );
		}
		visibleImg.update();
		_visibleDirty = false;
	}
}

inline const glm::vec3 ofxStructureCore::getGyroRotationRate()
{
	std::unique_lock<std::mutex> lck( _frameLock );
	auto r = _gyroscopeEvent.rotationRate();
	return {r.x, r.y, r.z};
}

inline const glm::vec3 ofxStructureCore::getAcceleration()
{
	std::unique_lock<std::mutex> lck( _frameLock );
	auto a = _accelerometerEvent.acceleration();
	return {a.x, a.y, a.z};
}

// static methods

std::vector<std::string> ofxStructureCore::listDevices( bool bLog )
{

	std::vector<std::string> devices;
	const ST::ConnectedSensorInfo* sensors[10];
	int count;
	ST::enumerateConnectedSensors( sensors, &count );
	std::stringstream devices_ss;
	for ( int i = 0; i < count; ++i ) {
		if ( sensors && sensors[i] ) {
			if ( bLog ) {
				devices_ss << "\n\t"
				           << "serial [" << sensors[i]->serial << "], "
				           << "product: " << sensors[i]->product << ", "
				           << std::boolalpha
				           << "available: " << sensors[i]->available << ", "
				           << "booted: " << sensors[i]->booted << std::endl;
			}
			devices.push_back( sensors[i]->serial );
		}
	}
	if ( bLog ) {
		ofLogNotice( ofx_module() ) << "Found " << devices.size() << " devices: " << devices_ss.str();
	}
	return devices;
}

// protected callback handlers -- not to be called directly:

inline void ofxStructureCore::handleNewFrame( const Frame& frame )
{
	std::stringstream data_ss;

	switch ( frame.type ) {
		case Frame::Type::DepthFrame: {
			std::unique_lock<std::mutex> lck( _frameLock );
			_depthFrame = frame.depthFrame;
			_depthDirty = true;  // update the pix/tex in update() loop
		} break;

		case Frame::Type::VisibleFrame: {
			std::unique_lock<std::mutex> lck( _frameLock );
			_visibleFrame = frame.visibleFrame;
			_visibleDirty = true;
		} break;

		case Frame::Type::InfraredFrame: {
			std::unique_lock<std::mutex> lck( _frameLock );
			_irFrame = frame.infraredFrame;
			_irDirty = true;
		} break;

		case Frame::Type::SynchronizedFrames: {
			if ( frame.depthFrame.isValid() ) {
				std::unique_lock<std::mutex> lck( _frameLock );
				_depthFrame = frame.depthFrame;
				_depthDirty = true;
			}
			if ( frame.visibleFrame.isValid() ) {
				std::unique_lock<std::mutex> lck( _frameLock );
				_visibleFrame = frame.visibleFrame;
				_visibleDirty = true;
			}
			if ( frame.infraredFrame.isValid() ) {
				std::unique_lock<std::mutex> lck( _frameLock );
				_irFrame = frame.infraredFrame;
				_irDirty = true;
			}
		} break;

		case Frame::Type::AccelerometerEvent: {
			std::unique_lock<std::mutex> lck( _frameLock );
			_accelerometerEvent = frame.accelerometerEvent;
		} break;

		case Frame::Type::GyroscopeEvent: {
			std::unique_lock<std::mutex> lck( _frameLock );
			_gyroscopeEvent = frame.gyroscopeEvent;
		} break;

		default: {
			ofLogWarning( ofx_module() ) << "Unhandled frame type: " << Frame::toString( frame.type );
		} break;
	}
	if (_depthDirty  && _depthProjectionMatrix == glm::mat4(1)) {
		_depthProjectionMatrix = glm::make_mat4(_depthFrame.glProjectionMatrix().m);
		_depthIntrinsics = _depthFrame.intrinsics();
		ofLogNotice() << "\n----------------------\n" << _depthProjectionMatrix
			<< "\n----------------------\n"
			<< "cx: " << _depthIntrinsics.cx << ", cy: " << _depthIntrinsics.cy << "\n"
			<< "fx: " << _depthIntrinsics.fx << ", fy: " << _depthIntrinsics.fy;
	}
}

inline void ofxStructureCore::handleSessionEvent( EventType evt )
{
	const std::string id = _captureSession.sensorInfo().serialNumber;
	switch ( evt ) {
		case ST::CaptureSessionEventId::Booting:
			ofLogVerbose( ofx_module() ) << "StructureCore is booting...";
			break;
		case ST::CaptureSessionEventId::Ready:
			ofLogNotice( ofx_module() ) << "Sensor " << id << " is ready.";
			if ( _streamOnReady ) {
				ofLogNotice( ofx_module() ) << "Sensor " << id << " is starting...";
				start();
			}
			break;
		case ST::CaptureSessionEventId::Connected:
			ofLogVerbose( ofx_module() ) << "Sensor " << id << " is connected.";
			break;
		case ST::CaptureSessionEventId::Streaming:
			ofLogVerbose( ofx_module() ) << "Sensor " << id << " is streaming.";
			_isStreaming = true;
			break;
		case ST::CaptureSessionEventId::Disconnected:
			ofLogError( ofx_module() ) << "Sensor " << id << " - Disconnected!";
			_isStreaming = false;
			break;
		case ST::CaptureSessionEventId::Error:
			ofLogError( ofx_module() ) << "Sensor " << id << " - Capture error!";
			break;
		default:
			ofLogWarning( ofx_module() ) << "Sensor " << id << " - Unhandled capture session event type: " << Frame::toString( evt );
	}
}

void ofxStructureCore::updatePointCloud()
{

	int cols     = depthImg.getWidth();
	int rows     = depthImg.getHeight();
	auto& depths = depthImg.getPixels();

	float _fx = _depthIntrinsics.fx;
	float _fy = _depthIntrinsics.fy;
	float _cx = _depthIntrinsics.cx;
	float _cy = _depthIntrinsics.cy;

	size_t nVerts = rows * cols;
	pointcloud.vertices.resize( nVerts );
	for ( int r = 0; r < rows; r++ ) {
		for ( int c = 0; c < cols; c++ ) {
			int i       = r * cols + c;
			float depth = depths[i];	// millimeters
			// project depth image into metric space
			// see: http://nicolas.burrus.name/index.php/Research/KinectCalibration
			pointcloud.vertices[i].x  = depth * ( c - _cx ) / _fx;
			pointcloud.vertices[i].y  = depth * ( r - _cy ) / _fy;
			pointcloud.vertices[i].z  = depth;
		}
	}
	vbo.setVertexData( pointcloud.vertices.data(), nVerts, GL_STATIC_DRAW );

	// test read back buffer:
	//auto data = vbo.getVertexBuffer().map<glm::vec3>(GL_READ_ONLY);
	//std::copy_n(data, nVerts, pointcloud.vertices.data());
	//vbo.getVertexBuffer().unmap();
	//vbo.setVertexData(pointcloud.vertices.data(), nVerts, GL_STATIC_DRAW);
}
