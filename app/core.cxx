#include "core.h"

DefineProtocol(NetProto1)

// V1
extern constexpr size_t ChunkSize = 512;

DefineProtocolVersion(NetProto1, NP1V1)
DefineProtocolMessage(NP1V1, NP1V1Clock, void(uint64_t InstanceID, uint64_t SystemTime))
DefineProtocolMessage(NP1V1, NP1V1Prepare, void(HashType MediaID, uint64_t Size))
DefineProtocolMessage(NP1V1, NP1V1Request, void(HashType MediaID, uint64_t From))
DefineProtocolMessage(NP1V1, NP1V1Data, void(HashType MediaID, uint64_t Chunk, std::vector<uint8_t> Bytes))
DefineProtocolMessage(NP1V1, NP1V1Play, void(HashType MediaID, uint64_t MediaTime, uint64_t SystemTime))
DefineProtocolMessage(NP1V1, NP1V1Stop, void(void))
DefineProtocolMessage(NP1V1, NP1V1Chat, void(std::string Message))

struct FilePieces
{
	FilePieces(uint64_t Size) : Runs{0, Size} {}

	bool Get(uint64_t Index)
	{
		bool Got = true;
		uint64_t Position = 0;
		for (auto const Length : Runs)
		{
			Position += Length;
			if (Index < Position) return Got;
			Got = !Got;
		}
		return false;
	}

	void Set(uint64_t Index)
	{
		std::vector<uint64_t> NewRuns;
		NewRuns.reserve(Runs.size() + 2);
		bool Got = true;
		bool Extend = false;
		uint64_t Position = 0;
		for (auto const Length : Runs)
		{
			if (!Got && (Index >= Position) && (Index < Position + Length))
			{
				if (Index == Position)
				{
					NewRuns.back() += 1;
					if (Length == 1) Extend = true;
					else NewRuns.push_back(Length - 1);
				}
				else if (Index == Position + Length - 1)
				{
					NewRuns.push_back(Length - 1);
					NewRuns.push_back(1);
					Extend = true;
				}
				else
				{
					NewRuns.push_back(Index - Position);
					NewRuns.push_back(1);
					NewRuns.push_back(Length - (Index - Position) - 1);
				}
			}
			else
			{
				assert(!Extend || (Got == Extend));
				if (Got && Extend)
					NewRuns.back() += Length;
				else NewRuns.push_back(Length);
			}
			Position += Length;
			Got = !Got;
		}
		Runs.swap(NewRuns);
	}

	std::vector<uint64_t> Runs;
};

struct Core;

struct CoreConnection : Network::Connection
{
	Core &Parent;

	struct MediaInfo
	{
		HashType ID;
		size_t Size;
	};

	std::queue<MediaInfo> Announce;

	struct
	{
		HashType ID;
		size_t Size;
		FilePieces Pieces;
		std::time_point LastResponse;
		bfs::path Path;
		std::fstream File;
	} Request;
	std::vector<MediaInfo> PendingRequests;
	struct FinishedMedia
	{
		HashType ID;
		size_t Size;
		bfs::path Path;
	};

	struct
	{
		HashType ID;
		std::fstream File;
		size_t Chunk;
	} DataStatus;

	CoreConnection(Core &Parent, std::string cosnt &Host, uint16_t Port, int Socket, ev_loop *EVLoop) :
		Network<CoreConnection>::Connection{Host, Port, Socket, EVLoop, this}, Parent(Parent) { }

	bool IdleWrite(CoreConnection &Info)
	{
		if (!Announce.empty())
		{
			Info.Send(NP1V1Prepare, Announce.front().ID, Announce.front().Size);
			Announce.pop();
			return true;
		}

		if (DataStatus.File)
		{
			std::vector<uint8_t> Data(ChunkSize);
			DataStatus.File.read(&Data[0], Data.size());
			Send(NP1V1Data, DataStatus.ID, Chunk++, Data);
			return true;
		}

		return false;
	}

	void HandleTimer(std::time_point const &Now)
	{
		Send(NP1V1Clock, Parent.ID, Now);

		if (Request.File.is_open() && ((Clock::now() - Request.LastResponse) > std::chrono::time(10)))
		Send(NP1V1Request, Connection.Request.ID, Connection.Request.Pieces.Next());
	}

	void Handle(NP1V1Clock, uint64_t const &InstanceID, uint64_t const &SystemTime)
	{
		Parent.Net.Forward(NP1V1Clock, State, InstanceID, SystemTime);
		if (Parent->ClockCallback) Parent->ClockCallback(InstanceID, SystemTime);
	}

	void Handle(NP1V1Prepare, HashType const &MediaID, uint64_t const &Size)
	{
		bool Found = Parent.Library.find(MediaID);
		if (Found != Parent.Library.end()) return;
		Parent.Net.Forward(NP1V1Prepare, *this, MediaID, Size);
		PendingRequests.emplace_back(MediaID, Size);
	}

	void Handle(NP1V1Request, HashType const &MediaID, uint64_t const &From)
	{
		auto Out = Parent.Library.find(MediaID);
		if (Out == Parent.Library.end()) return;
		if (!DataStatus.File.is_open() || (MediaID != DataStatus.ID))
			DataStatus.File.swap({Out->Path, std::fstream::in});
		DataStatus.File.seekg(From * ChunkSize);
		DataStatus.ID = MediaID;
		DataStatus.Chunk = From;
		WakeIdleWrite();
	}

	void Handle(NP1V1Data, HashType const &MediaID, uint64_t const &Chunk, std::vector<uint8_t> const &Bytes)
	{
		if (MediaID != Request.ID) return {};
		if (Chunk != Request.Pieces.Next()) return {};
		assert(Request.File);
		assert(Request.File.tellp() == Chunk * ChunkSize);
		if (Bytes.size() < ChunkSize || (Chunk * ChunkSize + Bytes.size() != Request.Size)) return {}; // Probably an error condition
		Request.Pieces.Set(Chunk);
		Request.File.write(&Bytes[0], ChunkSize);
		Request.LastResponse = Clock::now();
		if (!Request.Pieces.Finished()) return ;

		Request.File.close();
		Parent.Library.emplace(Request.ID, Request.Size, Request.Path);
		if (Parent.AddCallback) Parent.AddCallback(Request.ID, Request.Path);

		if (!PendingRequests.empty())
		{
			Request.ID = PendingRequests.front().ID;
			Request.Size = PendingRequests.front().Size;
			Request.Pieces = {};
			Request.Path = Root / FormatHash(Request.ID);
			Request.File = {Path, std::fstream::out};
			Send(NP1V1Request, Request.ID, Request.Pieces.Next());
		}
	}

	void Handle(NP1V1Play, HashType const &MediaID, uint64_t const &MediaTime, uint64_t const &SystemTime)
	{
		Parent.Net.Forward(NP1V1Play, this, MediaID, MediaTime, SystemTime);
		Parent.LastPlaying = true;
		Parent.LastMediaTime = MediaTime;
		ParentLastSystemTime = SystemTime;
		if (Parent.PlayCallback) Parent.PlayCallback(MediaID, MediaTime, SystemTime);
	}

	void Handle(NP1V1Stop)
	{
		Net.Forward(NP1V1Stop, Info);
		Parent.LastPlaying = false;
		if (StopCallback) StopCallback();
	}

	void Handle(NP1V1Chat, std::string const &Message)
	{
		Net.Forward(NP1V1Chat, State, Message);
		if (ChatCallback) ChatCallback(Message);
	}
};

Core::Core(bool Listen, std::string const &Host, uint8_t Port) :
	TempPath{bfs::temp_directory_path() / bfs::unique_path()},
	ID{std::uniform_int_distribution<uint64_t>{}(std::mt19937{})},
	Net<CoreConnection, NP1V1Clock, NP1V1Prepare, NP1V1Request, NP1V1Data, NP1V1Play, NP1V1Stop, NP1V1Chat>
	{
		[this](std::string const &Host, uint16_t Port, int Socket, ev_loop *EVLoop) // Create connection
		{
			auto Out = new CoreConnection{*this, Host, Port, Socket, EVLoop};
			for (auto Item : Parent.Library)
				Out->Announce.emplace(Item->first, Item->second.Size);
			return Out;
		},
		10.0f
	}
{
	Net.Open(Listen ? new CustomListener<CoreConnection>(Host, Port) : new CoreConnection(Host, Port));
}

Core::~Core(void)
{
	bfs::remove_all(TempPath);
}
