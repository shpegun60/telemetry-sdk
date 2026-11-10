/* Legacy TCP server compiles/links without Batch or Qt link roots. MIT. */
#include <integration/tcp/Server.hpp>
#include <array>

int main()
{
	constexpr telemetry::FieldCatalogTable fields{};
	constexpr telemetry::CommandCatalogTable commands{};
	constexpr telemetry::ServiceCatalogTable services{};
	const telemetry::Model model{fields, commands, services};
	std::array<std::byte, 256> scratch{}, rx{}, tx{};
	telemetry::Workspace workspace{scratch};
	telemetry::integration::tcp::Server server{model.view(), workspace, resource::FileSystemView{},
	                                           rx, tx};
	if (server.closed())
		return 1;
	server.close();
	return server.closed() ? 0 : 1;
}
