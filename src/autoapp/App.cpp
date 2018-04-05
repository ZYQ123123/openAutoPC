/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/

#include <thread>
#include <f1x/aasdk/USB/AOAPDevice.hpp>
#include <f1x/aasdk/TCP/TCPEndpoint.hpp>
#include <f1x/openauto/autoapp/App.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{

App::App(boost::asio::io_service& ioService, aasdk::usb::USBWrapper& usbWrapper, aasdk::tcp::ITCPWrapper& tcpWrapper, projection::IAndroidAutoEntityFactory& androidAutoEntityFactory,
         aasdk::usb::IUSBHub::Pointer usbHub, aasdk::usb::IConnectedAccessoriesEnumerator::Pointer connectedAccessoriesEnumerator)
    : ioService_(ioService)
    , usbWrapper_(usbWrapper)
    , tcpWrapper_(tcpWrapper)
    , strand_(ioService_)
    , androidAutoEntityFactory_(androidAutoEntityFactory)
    , usbHub_(std::move(usbHub))
    , connectedAccessoriesEnumerator_(std::move(connectedAccessoriesEnumerator))
    , isStopped_(false)
{

}

void App::waitForUSBDevice()
{
    strand_.dispatch([this, self = this->shared_from_this()]() {
        this->waitForDevice();
        this->enumerateDevices();
    });
}

void App::start(aasdk::tcp::ITCPEndpoint::SocketPointer socket)
{
    strand_.dispatch([this, self = this->shared_from_this(), socket = std::move(socket)]() mutable {
        if(androidAutoEntity_ == nullptr)
        {
            try
            {
                usbHub_->cancel();
                connectedAccessoriesEnumerator_->cancel();

                auto tcpEndpoint(std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper_, std::move(socket)));
                androidAutoEntity_ = androidAutoEntityFactory_.create(std::move(tcpEndpoint));
                androidAutoEntity_->start(*this);
            }
            catch(const aasdk::error::Error& error)
            {
                OPENAUTO_LOG(error) << "[App] TCP AndroidAutoEntity create error: " << error.what();

                androidAutoEntity_.reset();
                this->waitForDevice();
            }
        }
        else
        {
            tcpWrapper_.close(*socket);
            OPENAUTO_LOG(warning) << "[App] android auto entity is still running.";
        }
    });
}

void App::stop()
{
    strand_.dispatch([this, self = this->shared_from_this()]() {
        isStopped_ = true;
        connectedAccessoriesEnumerator_->cancel();
        usbHub_->cancel();

        if(androidAutoEntity_ != nullptr)
        {
            androidAutoEntity_->stop();
        }
    });
}

void App::aoapDeviceHandler(aasdk::usb::DeviceHandle deviceHandle)
{
    OPENAUTO_LOG(info) << "[App] Device connected.";
#ifdef RASPBERRYPI3
    system("/opt/crankshaft/dumb_suid usb_plug.sh &");
    OPENAUTO_LOG(info) << "[CS] Ran USB Plug script.";
#endif

    if(androidAutoEntity_ == nullptr)
    {
        try
        {
            connectedAccessoriesEnumerator_->cancel();

            auto aoapDevice(aasdk::usb::AOAPDevice::create(usbWrapper_, ioService_, deviceHandle));
            androidAutoEntity_ = androidAutoEntityFactory_.create(std::move(aoapDevice));
            androidAutoEntity_->start(*this);
        }
        catch(const aasdk::error::Error& error)
        {
            OPENAUTO_LOG(error) << "[App] USB AndroidAutoEntity create error: " << error.what();

            androidAutoEntity_.reset();
            this->waitForDevice();
        }
    }
    else
    {
        OPENAUTO_LOG(warning) << "[App] android auto entity is still running.";
    }
}

void App::enumerateDevices()
{
    auto promise = aasdk::usb::IConnectedAccessoriesEnumerator::Promise::defer(strand_);
    promise->then([this, self = this->shared_from_this()](auto result) {
            OPENAUTO_LOG(info) << "[App] Devices enumeration result: " << result;
        },
        [this, self = this->shared_from_this()](auto e) {
            OPENAUTO_LOG(error) << "[App] Devices enumeration failed: " << e.what();
        });

    connectedAccessoriesEnumerator_->enumerate(std::move(promise));
}

void App::waitForDevice()
{
    OPENAUTO_LOG(info) << "[App] Waiting for device...";

    auto promise = aasdk::usb::IUSBHub::Promise::defer(strand_);
    promise->then(std::bind(&App::aoapDeviceHandler, this->shared_from_this(), std::placeholders::_1),
                  std::bind(&App::onUSBHubError, this->shared_from_this(), std::placeholders::_1));
    usbHub_->start(std::move(promise));
}

void App::onAndroidAutoQuit()
{
    strand_.dispatch([this, self = this->shared_from_this()]() {
        OPENAUTO_LOG(info) << "[App] quit.";

        androidAutoEntity_->stop();
        androidAutoEntity_.reset();

#ifdef RASPBERRYPI3
        system("/opt/crankshaft/dumb_suid usb_unplug.sh &");
        OPENAUTO_LOG(info) << "[CS] Ran USB Unplug script.";
#endif

        if(!isStopped_)
        {
            this->waitForDevice();
        }
    });
}

void App::onUSBHubError(const aasdk::error::Error& error)
{
    OPENAUTO_LOG(error) << "[App] usb hub error: " << error.what();

    if(error != aasdk::error::ErrorCode::OPERATION_ABORTED &&
       error != aasdk::error::ErrorCode::OPERATION_IN_PROGRESS)
    {
        this->waitForDevice();
    }
}

}
}
}
