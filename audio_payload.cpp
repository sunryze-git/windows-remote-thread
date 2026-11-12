// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <propvarutil.h>
#include <shlwapi.h>

namespace
{

template <typename T>
class com_ptr
{
public:
	com_ptr() = default;
	com_ptr(const com_ptr &) = delete;
	com_ptr & operator=(const com_ptr &) = delete;

	~com_ptr()
	{
		reset();
	}

	T * get() const
	{
		return ptr;
	}

	T * operator->() const
	{
		return ptr;
	}

	explicit operator bool() const
	{
		return ptr != nullptr;
	}

	T ** put()
	{
		reset();
		return &ptr;
	}

	T * detach()
	{
		T * value = ptr;
		ptr = nullptr;
		return value;
	}

	void reset()
	{
		if (ptr != nullptr)
		{
			ptr->Release();
			ptr = nullptr;
		}
	}

private:
	T * ptr = nullptr;
};

HRESULT build_audio_topology(IMFMediaSource * source, IMFTopology ** out)
{
	com_ptr<IMFPresentationDescriptor> presentation;
	HRESULT hr = source->CreatePresentationDescriptor(presentation.put());
	if (FAILED(hr))
		return hr;

	DWORD stream_count = 0;
	hr = presentation->GetStreamDescriptorCount(&stream_count);
	if (FAILED(hr))
		return hr;

	for (DWORD index = 0; index < stream_count; ++index)
	{
		BOOL selected = FALSE;
		com_ptr<IMFStreamDescriptor> stream;
		hr = presentation->GetStreamDescriptorByIndex(index, &selected, stream.put());
		if (FAILED(hr))
			return hr;

		com_ptr<IMFMediaTypeHandler> handler;
		hr = stream->GetMediaTypeHandler(handler.put());
		if (FAILED(hr))
			return hr;

		GUID major_type{};
		hr = handler->GetMajorType(&major_type);
		if (FAILED(hr))
			return hr;

		if (major_type != MFMediaType_Audio)
			continue;

		if (!selected)
		{
			hr = presentation->SelectStream(index);
			if (FAILED(hr))
				return hr;
		}

		com_ptr<IMFTopologyNode> source_node;
		hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, source_node.put());
		if (FAILED(hr))
			return hr;

		hr = source_node->SetUnknown(MF_TOPONODE_SOURCE, source);
		if (FAILED(hr))
			return hr;

		hr = source_node->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, presentation.get());
		if (FAILED(hr))
			return hr;

		hr = source_node->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, stream.get());
		if (FAILED(hr))
			return hr;

		com_ptr<IMFActivate> renderer;
		hr = MFCreateAudioRendererActivate(renderer.put());
		if (FAILED(hr))
			return hr;

		com_ptr<IMFTopologyNode> output_node;
		hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, output_node.put());
		if (FAILED(hr))
			return hr;

		hr = output_node->SetObject(renderer.get());
		if (FAILED(hr))
			return hr;

		hr = output_node->SetUINT32(MF_TOPONODE_STREAMID, 0);
		if (FAILED(hr))
			return hr;

		hr = output_node->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);
		if (FAILED(hr))
			return hr;

		com_ptr<IMFTopology> topology;
		hr = MFCreateTopology(topology.put());
		if (FAILED(hr))
			return hr;

		hr = topology->AddNode(source_node.get());
		if (FAILED(hr))
			return hr;

		hr = topology->AddNode(output_node.get());
		if (FAILED(hr))
			return hr;

		hr = source_node->ConnectOutput(0, output_node.get(), 0);
		if (FAILED(hr))
			return hr;

		*out = topology.detach();
		return S_OK;
	}

	return E_FAIL;
}

HRESULT play_audio(const wchar_t * path)
{
	if (path == nullptr || path[0] == L'\0')
		return E_INVALIDARG;

	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool com_initialized = SUCCEEDED(hr);
	if (hr == RPC_E_CHANGED_MODE)
		hr = S_OK;
	else if (FAILED(hr))
		return hr;

	hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr))
	{
		if (com_initialized)
			CoUninitialize();
		return hr;
	}

	com_ptr<IMFMediaSession> session;
	hr = MFCreateMediaSession(nullptr, session.put());
	if (FAILED(hr))
		goto cleanup;

	{
		wchar_t url[32768];
		DWORD url_length = static_cast<DWORD>(sizeof(url) / sizeof(url[0]));
		hr = UrlCreateFromPathW(path, url, &url_length, URL_ESCAPE_PERCENT);
		if (FAILED(hr))
			goto cleanup;

		com_ptr<IMFSourceResolver> resolver;
		hr = MFCreateSourceResolver(resolver.put());
		if (FAILED(hr))
			goto cleanup;

		MF_OBJECT_TYPE object_type = MF_OBJECT_INVALID;
		com_ptr<IUnknown> source_unknown;
		hr = resolver->CreateObjectFromURL(url, MF_RESOLUTION_MEDIASOURCE, nullptr, &object_type, source_unknown.put());
		if (FAILED(hr))
			goto cleanup;

		com_ptr<IMFMediaSource> media_source;
		hr = source_unknown->QueryInterface(IID_IMFMediaSource, reinterpret_cast<void **>(media_source.put()));
		if (FAILED(hr))
			goto cleanup;

		com_ptr<IMFTopology> topology;
		hr = build_audio_topology(media_source.get(), topology.put());
		if (FAILED(hr))
			goto cleanup;

		hr = session->SetTopology(0, topology.get());
		if (FAILED(hr))
			goto cleanup;

		PROPVARIANT start_position{};
		hr = session->Start(&GUID_NULL, &start_position);
		PropVariantClear(&start_position);
		if (FAILED(hr))
			goto cleanup;
	}

	{
		bool done = false;
		while (!done)
		{
			com_ptr<IMFMediaEvent> event;
			hr = session->GetEvent(0, event.put());
			if (FAILED(hr))
				break;

			MediaEventType type = MEUnknown;
			hr = event->GetType(&type);
			if (FAILED(hr))
				break;

			switch (type)
			{
				case MESessionEnded:
				case MESessionClosed:
					done = true;
					break;

				case MEError:
				{
					HRESULT status = S_OK;
					if (FAILED(event->GetStatus(&status)))
						hr = E_FAIL;
					else if (FAILED(status))
						hr = status;
					done = true;
					break;
				}

				case MESessionTopologyStatus:
				{
					UINT32 status = 0;
					if (SUCCEEDED(event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &status)) && status == MF_TOPOSTATUS_INVALID)
					{
						hr = E_FAIL;
						done = true;
					}
					break;
				}

				default:
					break;
			}
		}
	}

cleanup:
	if (session)
	{
		session->Shutdown();
		session.reset();
	}

	MFShutdown();

	if (com_initialized)
		CoUninitialize();

	return hr;
}

}

extern "C" __declspec(dllexport) HRESULT __stdcall PlayAudioFileW(const wchar_t * path)
{
	return play_audio(path);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);

	return TRUE;
}
