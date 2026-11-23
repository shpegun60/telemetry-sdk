/*
 * @file QtSmoke.cpp
 * @brief Qt C++20 integration with the same Model and encoded fake Device.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * Exercise the browser example Model from Qt using canonical binary payloads.
 * QByteArray owns the byte buffers and Workspace owns decoded object lifetimes;
 * the JSON summary uses strings where Qt numbers would lose integer bits.
 */

#include "Device.hpp"
#include <QCoreApplication>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace ex = client_example;
namespace ts = telemetry;

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	QByteArray input(static_cast<qsizetype>(ts::wireSize<ex::Request>), Qt::Uninitialized);
	QByteArray output(static_cast<qsizetype>(ts::wireSize<ex::Response>), Qt::Uninitialized);
	const auto inputBytes = std::span{reinterpret_cast<std::byte*>(input.data()),
	                                  static_cast<std::size_t>(input.size())};
	const auto outputBytes = std::span{reinterpret_cast<std::byte*>(output.data()),
	                                   static_cast<std::size_t>(output.size())};
	if (ts::encode(ex::request, inputBytes) != ts::CodecStatus::Ok)
		return 1;
	const auto result =
	    ex::model.serviceIndex().callEncoded(ts::idOf<ex::model, "device/Inspect">(), inputBytes,
	                                    outputBytes, ex::workspace);
	if (result.dispatch != ts::DispatchStatus::Ok ||
	    result.endpointStatus != ts::ServiceStatus::Ok || result.written != outputBytes.size() ||
	    ex::device.calls != 1)
		return 2;
	auto lease = ex::workspace.reserve<ex::Response>();
	ex::Response* response = nullptr;
	if (ts::decode<ex::Response>(outputBytes, lease, response) != ts::CodecStatus::Ok)
		return 3;
	if (response->config.serial != UINT64_MAX || response->config.offset != INT64_MIN ||
	    response->config.mode != static_cast<ex::Mode>(-2) || response->checksum != 263168)
		return 4;
	const auto native = ex::model.fields().readAs<ex::Config>(ts::idOf<ex::model, "device/Config">());
	if (!native || native->serial != response->config.serial)
		return 5;
	// Qt JSON has no lossless integer64 number. Decimal strings are a desktop
	// display choice; the actual request/response stays canonical binary.
	const QJsonObject summary{
	    {"serial", QString::number(response->config.serial)},
	    {"offset", QString::number(response->config.offset)},
	    {"checksum", static_cast<qint64>(response->checksum)},
	    {"fingerprint", QString::number(ex::descriptor.fingerprint(), 16)},
	    {"wireBytes", static_cast<qint64>(result.written)},
	};
	QTextStream(stdout) << QJsonDocument(summary).toJson(QJsonDocument::Compact) << '\n';
	return 0;
}
