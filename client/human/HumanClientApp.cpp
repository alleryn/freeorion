#include "HumanClientApp.h"

#include "../../UI/CUIControls.h"
#include "dialogs/GGFileDlg.h"
#include "../../UI/MapWnd.h"
#include "../../network/Message.h"
#include "../../UI/MultiplayerLobbyWnd.h"
#include "../../util/OptionsDB.h"
#include "../../universe/Planet.h"
#include "../../util/Process.h"
#include "../../util/SitRepEntry.h"
#include "XMLDoc.h"

#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include <sstream>

namespace {
    GG::Wnd* NewCUIButton(const GG::XMLElement& elem)         {return new CUIButton(elem);}
    GG::Wnd* NewCUIStateButton(const GG::XMLElement& elem)    {return new CUIStateButton(elem);}
    GG::Wnd* NewCUIScroll(const GG::XMLElement& elem)         {return new CUIScroll(elem);}
    GG::Wnd* NewCUIScrollTab(const GG::XMLElement& elem)      {return new CUIScroll::ScrollTab(elem);}
    GG::Wnd* NewCUIListBox(const GG::XMLElement& elem)        {return new CUIListBox(elem);}
    GG::Wnd* NewCUIDropDownList(const GG::XMLElement& elem)   {return new CUIDropDownList(elem);}
    GG::Wnd* NewCUIEdit(const GG::XMLElement& elem)           {return new CUIEdit(elem);}
    GG::Wnd* NewCUIMultiEdit(const GG::XMLElement& elem)      {return new CUIMultiEdit(elem);}
}
 
HumanClientApp::HumanClientApp() : 
    ClientApp(), 
    SDLGGApp(GetOptionsDB().Get<int>("app-width"), 
             GetOptionsDB().Get<int>("app-height"),
             false, "freeorion"),
    m_current_music(0),
    m_single_player_game(true),
    m_game_started(false)
{
    AddWndGenerator("CUIButton", &NewCUIButton);
    AddWndGenerator("CUIStateButton", &NewCUIStateButton);
    AddWndGenerator("CUIScroll", &NewCUIScroll);
    AddWndGenerator("CUIScroll::ScrollTab", &NewCUIScrollTab);
    AddWndGenerator("CUIListBox", &NewCUIListBox);
    AddWndGenerator("CUIDropDownList", &NewCUIDropDownList);
    AddWndGenerator("CUIEdit", &NewCUIEdit);
    AddWndGenerator("CUIMultiEdit", &NewCUIMultiEdit);

    SetMaxFPS(60.0);
}

HumanClientApp::~HumanClientApp()
{
}

Message HumanClientApp::TurnOrdersMessage(bool save_game_data/* = false*/) const
{
    GG::XMLDoc orders_doc;
    if (save_game_data) {
        orders_doc.root_node.AppendChild("save_game_data");
        orders_doc.root_node.AppendChild(GetUI()->SaveGameData()); // include relevant UI state
    }
    orders_doc.root_node.AppendChild(GG::XMLElement("Orders"));
    for (OrderSet::const_iterator order_it = m_orders.begin(); order_it != m_orders.end(); ++order_it) {
        orders_doc.root_node.LastChild().AppendChild(order_it->second->XMLEncode());
    }
    return ::TurnOrdersMessage(m_player_id, -1, orders_doc);
}

void HumanClientApp::StartServer()
{
#ifdef FREEORION_WIN32
    const std::string SERVER_CLIENT_EXE = "freeoriond.exe";
#else
    const std::string SERVER_CLIENT_EXE = "freeoriond";
#endif
    std::vector<std::string> args(1, SERVER_CLIENT_EXE);
    args.push_back("--data-dir"); args.push_back(GetOptionsDB().Get<std::string>("data-dir"));
    m_server_process = Process(SERVER_CLIENT_EXE, args);
}

void HumanClientApp::FreeServer()
{
    m_server_process.Free();
    m_player_id = -1;
    m_empire_id = -1;
    m_player_name = "";
}

void HumanClientApp::KillServer()
{
    m_server_process.Kill();
    m_player_id = -1;
    m_empire_id = -1;
    m_player_name = "";
}

void HumanClientApp::EndGame()
{
    m_game_started = false;
    m_network_core.DisconnectFromServer();
    m_server_process.RequestTermination();
    m_player_id = -1;
    m_empire_id = -1;
    m_player_name = "";
    GetUI()->GetMapWnd()->CloseAllPopups();
    GetUI()->ScreenIntro();
}

void HumanClientApp::SetLobby(MultiplayerLobbyWnd* lobby)
{
    m_multiplayer_lobby_wnd = lobby;
}

void HumanClientApp::PlayMusic(const std::string& filename, int repeats, int ms/* = 0*/, double position/* = 0.0*/)
{
	if (repeats == -1) 
		repeats = -2;
    if (m_current_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(m_current_music);
        m_current_music = 0;
    }
    m_current_music = Mix_LoadMUS(filename.c_str());
    if (m_current_music) {
        if (Mix_PlayMusic(m_current_music, repeats + 1) == -1) {
            Mix_HaltMusic();
            Mix_FreeMusic(m_current_music);
            m_current_music = 0;
            Logger().errorStream() << "HumanClientApp::PlayMusic : An error occured while attempting to play \"" << 
                filename << "\"; SDL_mixer error: " << Mix_GetError();
        }
    } else {
        Logger().errorStream() << "HumanClientApp::PlayMusic : An error occured while attempting to load \"" << 
            filename << "\"; SDL_mixer error: " << Mix_GetError();
    }
}

void HumanClientApp::StartMusic(void)
{
	HumanClientApp::GetApp()->StopMusic();
	HumanClientApp::GetApp()->PlayMusic(ClientUI::MUSIC_DIR + GetOptionsDB().Get<std::string>("bg-music"), -1, 0, 0.0);
}

void HumanClientApp::StopMusic(void)
{
	Mix_HaltMusic();
	Mix_FreeMusic(m_current_music);
	m_current_music = 0;
}

void HumanClientApp::PlaySound(const std::string& filename, int repeats, int timeout/* = -1*/)
{
    // load and cache the sound data
    std::map<std::string, Mix_Chunk*>::iterator it = m_sounds.find(filename);
    if (it == m_sounds.end()) {
        Mix_Chunk* data = Mix_LoadWAV(filename.c_str());
        if (!data) {
            Logger().errorStream() << "HumanClientApp::PlaySound : An error occured while attempting to load \"" << 
                filename << "\"; SDL_mixer error: " << Mix_GetError();
            return;
        } else {
            m_sounds[filename] = data;
        }
    }

    // find a free channel, creating an additional channel if needed
    Mix_Chunk* data = m_sounds[filename];
    int channel = 0;
    int num_channels = Mix_AllocateChannels(-1);
    for (; channel < num_channels; ++channel) {
        if (m_channels[channel] == "")
            break;
    }
    // there are not enough channels, so create one
    if (channel == num_channels) {
        Mix_AllocateChannels(channel);
        m_channels.resize(channel);
    }

    // play
    if (Mix_PlayChannel(channel, data, repeats) != channel) {
        Logger().errorStream() << "HumanClientApp::PlaySound : An error occured while attempting to play \"" << 
            filename << "\"; SDL_mixer error: " << Mix_GetError();
    } else {
        m_channels[channel] = filename;
    }
}

void HumanClientApp::FreeSound(const std::string& filename)
{
    if (m_sounds.find(filename) != m_sounds.end()) {
        bool still_playing = false;
        for (unsigned int i = 0; i < m_channels.size(); ++i) {
            if (m_channels[i] == filename) {
                still_playing = true;
                break;
            }
        }
        if (!still_playing) {
            Mix_FreeChunk(m_sounds[filename]);
            m_sounds.erase(filename);
            m_sounds_to_free.erase(filename);
        } else {
            m_sounds_to_free.insert(filename);
        }
    }
}
   
void HumanClientApp::FreeAllSounds()
{
    for (std::map<std::string, Mix_Chunk*>::iterator it = m_sounds.begin(); it != m_sounds.end();) {
        std::map<std::string, Mix_Chunk*>::iterator temp = it++;
        FreeSound(temp->first);
    }
}

bool HumanClientApp::LoadSinglePlayerGame()
{
    if (!HumanClientApp::GetApp()->NetworkCore().Connected()) {
        if (!GetOptionsDB().Get<bool>("force-external-server"))
            HumanClientApp::GetApp()->StartServer();

        bool failed = false;
        int start_time = GG::App::GetApp()->Ticks();
        const int SERVER_CONNECT_TIMEOUT = 30000; // in ms
        while (!HumanClientApp::GetApp()->NetworkCore().ConnectToLocalhostServer()) {
            if (SERVER_CONNECT_TIMEOUT < GG::App::GetApp()->Ticks() - start_time) {
                ClientUI::MessageBox(ClientUI::String("ERR_CONNECT_TIMED_OUT"));
                failed = true;
                break;
            }
        }

        if (failed) {
            KillServer();
            return false;
        }

        // HACK!  send the multiplayer form of the HostGameMessage, since it establishes us as the host, and the single-player 
        // LOAD_GAME message will establish us as a single-player game
        GG::XMLDoc parameters;
        parameters.root_node.AppendChild(GG::XMLElement("host_player_name", std::string("Happy_Player")));
        if (!failed)
            HumanClientApp::GetApp()->NetworkCore().SendMessage(HostGameMessage(HumanClientApp::GetApp()->PlayerID(), parameters));
    }

    std::vector<std::pair<std::string, std::string> > save_file_types;
    save_file_types.push_back(std::pair<std::string, std::string>(ClientUI::String("INGAMEOPTIONS_SAVE_FILES"), "*.sav"));

    try {
        GG::FileDlg dlg(GetOptionsDB().Get<std::string>("save-dir"), "", false, false, save_file_types, 
                        ClientUI::FONT, ClientUI::PTS, ClientUI::WND_COLOR, ClientUI::WND_OUTER_BORDER_COLOR, ClientUI::TEXT_COLOR);
        dlg.Run();
        std::string filename;
        if (!dlg.Result().empty()) {
            filename = *dlg.Result().begin();

            m_ui->ScreenLoad();
            HumanClientApp::GetApp()->NetworkCore().SendMessage(HostLoadGameMessage(HumanClientApp::GetApp()->PlayerID(), filename));
            return true;
        } else {
            KillServer();
        }
    } catch (const GG::FileDlg::InitialDirectoryDoesNotExistException& e) {
        ClientUI::MessageBox(e.Message());
    }
    return false;
}

void HumanClientApp::Enter2DMode()
{
    glPushAttrib(GL_ENABLE_BIT | GL_PIXEL_MODE_BIT | GL_TEXTURE_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, AppWidth(), AppHeight()); //removed -1 from AppWidth & Height

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();

    // set up coordinates with origin in upper-left and +x and +y directions right and down, respectively
    // the depth of the viewing volume is only 1 (from 0.0 to 1.0)
    glOrtho(0.0, AppWidth(), AppHeight(), 0.0, 0.0, AppWidth());

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

void HumanClientApp::Exit2DMode()
{
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glPopAttrib();
}

HumanClientApp* HumanClientApp::GetApp()
{
    return dynamic_cast<HumanClientApp*>(GG::App::GetApp());
}

boost::shared_ptr<ClientUI> HumanClientApp::GetUI()
{
    return (dynamic_cast<HumanClientApp*>(GG::App::GetApp()))->m_ui;
}
   
void HumanClientApp::SDLInit()
{
    const SDL_VideoInfo* vid_info = 0;
    Uint32 DoFullScreen = 0;

    // Set Fullscreen if specified at command line or in config-file
    DoFullScreen = GetOptionsDB().Get<bool>("fullscreen") ? SDL_FULLSCREEN : 0;

#ifdef FREEORION_WIN32
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE) < 0) {
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE | SDL_INIT_EVENTTHREAD) < 0) {
#endif
        Logger().errorStream() << "SDL initialization failed: " << SDL_GetError();
        Exit(1);
    }

    SDL_WM_SetCaption("FreeOrion v0.2", "FreeOrion v0.2");

    int freq = MIX_DEFAULT_FREQUENCY; // sampling frequency
    Uint16 format = MIX_DEFAULT_FORMAT;
    int channels = 2; // stereo
    int chunk_sz = 2048;
    if (Mix_OpenAudio(freq, format, channels, chunk_sz)) {
        Logger().fatalStream() << "SDL Mixer initialization failed with parameters (frequency= " << freq << 
            ", format= " << format << ", channels= " << channels << ", chunksize= " << chunk_sz << "): " << Mix_GetError();
        Exit(1);
    } else {
        // ensure the correct runtime is being used      
        const SDL_version* link_version = Mix_Linked_Version();
        SDL_version compile_version;
        MIX_VERSION(&compile_version);
        if (compile_version.major != link_version->major || compile_version.minor != link_version->minor || 
            compile_version.patch != link_version->patch) {
            Logger().fatalStream() << "Version of SDL Mixer headers compiled with this program (v" << 
                compile_version.major << "." << compile_version.minor << "." << compile_version.patch << 
                ") does not match version in runtime library (v" <<
                link_version->major << "." << link_version->minor << "." << link_version->patch << ")";
            Exit(1);
        }      

        // check to see what values are actually being used, in case we didn't get what we wanted from initialization
        int actual_freq;
        Uint16 actual_format;
        int actual_channels;
        Mix_QuerySpec(&actual_freq, &actual_format, &actual_channels);
        if (freq != actual_freq) {
            Logger().debugStream() << "WARNING: SDL Mixer initialization was attempted with frequency= " << freq << ", but"
                "the actual frequency being used is " << actual_freq;
        }
        if (format != actual_format) {
            Logger().debugStream() << "WARNING: SDL Mixer initialization was attempted with format= " << format << ", but"
                "the actual format being used is " << actual_format;
        }
        if (channels != actual_channels) {
            Logger().debugStream() << "WARNING: SDL Mixer initialization was attempted in " << 
                (channels == 1 ? "mono" : "stereo") << ", but " << 
                (actual_channels == 1 ? "mono" : "stereo") << " is being used";
        }
        Mix_HookMusicFinished(&HumanClientApp::EndOfMusicCallback);
        Mix_ChannelFinished(&HumanClientApp::EndOfSoundCallback);
        m_channels.resize(actual_channels, "");
    }

    if (SDLNet_Init() < 0) {
        Logger().errorStream() << "SDL Net initialization failed: " << SDLNet_GetError();
        Exit(1);
    }

    if (FE_Init() < 0) {
        Logger().errorStream() << "FastEvents initialization failed: " << FE_GetError();
        Exit(1);
    }

    vid_info = SDL_GetVideoInfo();

    if (!vid_info) {
        Logger().errorStream() << "Video info query failed: " << SDL_GetError();
        Exit(1);
    }

    int bpp = boost::lexical_cast<int>(GetOptionsDB().Get<int>("color-depth"));
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (24 <= bpp) {
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    } else { // assumes 16 bpp minimum
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    }

    if (SDL_SetVideoMode(AppWidth(), AppHeight(), bpp, DoFullScreen|SDL_OPENGL) == 0) {
        Logger().errorStream() << "Video mode set failed: " << SDL_GetError();
        Exit(1);
    }

    if (NET2_Init() < 0) {
        Logger().errorStream() << "SDL Net2 initialization failed: " << NET2_GetError();
        Exit(1);
    }

    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
    EnableMouseDragRepeat(SDL_DEFAULT_REPEAT_DELAY / 2, SDL_DEFAULT_REPEAT_INTERVAL / 2);

    Logger().debugStream() << "SDLInit() complete.";
    GLInit();
}

void HumanClientApp::GLInit()
{
    double ratio = AppWidth() / (float)(AppHeight());

    glEnable(GL_BLEND);
    glClearColor(0, 0, 0, 0);
    glViewport(0, 0, AppWidth(), AppHeight());
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(50.0, ratio, 0.0, 10.0);
    gluLookAt(0.0, 0.0, 5.0, 
              0.0, 0.0, 0.0, 
              0.0, 1.0, 0.0);

    Logger().debugStream() << "GLInit() complete.";
}

void HumanClientApp::Initialize()
{
    m_ui = boost::shared_ptr<ClientUI>(new ClientUI());
    m_ui->ScreenIntro();    //start the first screen; the UI takes over from there.
	
	if (!(GetOptionsDB().Get<bool>("music-off")))
		HumanClientApp::GetApp()->StartMusic();
}

void HumanClientApp::HandleNonGGEvent(const SDL_Event& event)
{
    switch(event.type) {
        case SDL_USEREVENT: {
            int net2_type = NET2_GetEventType(const_cast<SDL_Event*>(&event));
            if (net2_type == NET2_ERROREVENT || 
                net2_type == NET2_TCPACCEPTEVENT || 
                net2_type == NET2_TCPRECEIVEEVENT || 
                net2_type == NET2_TCPCLOSEEVENT || 
                net2_type == NET2_UDPRECEIVEEVENT)
                m_network_core.HandleNetEvent(const_cast<SDL_Event&>(event));
            break;
        }

        case SDL_QUIT: {
            Exit(0);
            return;
        }
    }
}

void HumanClientApp::FinalCleanup()
{
    if (m_current_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(m_current_music);
        m_current_music = 0;
    }
    Mix_HaltChannel(-1); // stop all sound playback
    for (std::map<std::string, Mix_Chunk*>::iterator it = m_sounds.begin(); it != m_sounds.end(); ++it) {
        Mix_FreeChunk(it->second);
    }

    if (NetworkCore().Connected()) {
        NetworkCore().DisconnectFromServer();
    }
    m_server_process.RequestTermination();
}

void HumanClientApp::SDLQuit()
{
    FinalCleanup();
    NET2_Quit();
    FE_Quit();
    SDLNet_Quit();
    Mix_CloseAudio();   
    SDL_Quit();
    Logger().debugStream() << "SDLQuit() complete.";
}

void HumanClientApp::HandleMessageImpl(const Message& msg)
{
    switch (msg.Type()) {
    case Message::SERVER_STATUS: {
        std::stringstream stream(msg.GetText());
        GG::XMLDoc doc;
        doc.ReadDoc(stream);
        if (doc.root_node.ContainsChild("new_name")) {
            m_player_name = doc.root_node.Child("new_name").Text();
            Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Received SERVER_STATUS -- server has renamed this player \"" << 
                m_player_name  << "\"";
        } else if (doc.root_node.ContainsChild("server_state")) {
            ServerState server_state = ServerState(boost::lexical_cast<int>(doc.root_node.Child("server_state").Attribute("value")));
            Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Received SERVER_STATUS (status code " << 
                doc.root_node.Child("server_state").Attribute("value") << ")";
            if (server_state == SERVER_DYING)
                KillServer();
        }
        break;
    } 

    case Message::HOST_GAME: {
        if (msg.Sender() == -1 && msg.GetText() == "ACK")
            Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Received HOST_GAME acknowledgement";
        break;
    } 

    case Message::JOIN_GAME: {
        if (msg.Sender() == -1) {
            if (m_player_id == -1) {
                m_player_id = boost::lexical_cast<int>(msg.GetText());
                Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Received JOIN_GAME acknowledgement "
                    "(joined as player " << m_player_id << ")";
            } else {
                Logger().errorStream() << "HumanClientApp::HandleMessageImpl : Received erroneous JOIN_GAME acknowledgement when "
                    "already in a game";
            }
        }
        break;
    }

    case Message::GAME_START: {
        if (msg.Sender() == -1) {
            Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Received GAME_START message; "
                "starting player turn...";
            m_game_started = true;

            std::stringstream stream(msg.GetText());
            GG::XMLDoc doc;
            doc.ReadDoc(stream);

#if 0
            // dump the game start doc
            std::ofstream output("start_doc.txt");
            doc.WriteDoc(output);
            output.close();
#endif

            if (m_single_player_game = doc.root_node.ContainsChild("single_player_game")) {
                Logger().debugStream() << "Single-Player game";
                doc.root_node.RemoveChild("single_player_game");
            }

            m_empire_id = boost::lexical_cast<int>(doc.root_node.Child("empire_id").Text());

            m_universe.SetUniverse(doc.root_node.Child("Universe"));

            // free current sitreps, if any
            if (Empires().Lookup(m_empire_id))
                Empires().Lookup(m_empire_id)->ClearSitRep();

            Orders().Reset();

            // if we have empire data, then process it.  As it stands now,
            // we may not, so dont assume we do.
            if (doc.root_node.ContainsChild(EmpireManager::EMPIRE_UPDATE_TAG)) {
                m_empires.HandleEmpireElementUpdate(doc.root_node.Child(EmpireManager::EMPIRE_UPDATE_TAG));
            } else {
                Logger().debugStream() << "No Empire data received from server.  Update Server Code.";
            }
            
            Logger().debugStream() << "HumanClientApp::HandleMessageImpl : Universe setup complete.";

            for (Empire::SitRepItr it = Empires().Lookup(m_empire_id)->SitRepBegin(); it != Empires().Lookup(m_empire_id)->SitRepEnd(); ++it) {
                m_ui->GenerateSitRepText(*it);
            }

            int turn_number;
            turn_number = boost::lexical_cast<int>(doc.root_node.Attribute("turn_number"));

            m_ui->ScreenMap();
            m_ui->InitTurn( turn_number ); // init the new turn
        }
        break;
    }

    case Message::SAVE_GAME: {
        NetworkCore().SendMessage(TurnOrdersMessage(true));
        break;
    }

    case Message::LOAD_GAME: {
        std::stringstream stream(msg.GetText());
        GG::XMLDoc doc;
        doc.ReadDoc(stream);

        // re-issue orders given earlier in the saved turn
        GG::XMLObjectFactory<Order> factory;
        Order::InitOrderFactory(factory);
        for (int i = 0; i < doc.root_node.Child("Orders").NumChildren(); ++i) {
            Orders().IssueOrder(factory.GenerateObject(doc.root_node.Child("Orders").Child(i)));
        }

        // restore UI state
        if (doc.root_node.ContainsChild("UI")) {
            GetUI()->RestoreFromSaveData(doc.root_node.Child("UI"));
        }
        break;
    }

    case Message::TURN_UPDATE: {
        int turn_number;

        std::stringstream stream(msg.GetText());
        GG::XMLDoc doc;
        doc.ReadDoc(stream);

#if 0
        // dump the update doc
        std::ofstream output("update_doc.txt");
        doc.WriteDoc(output);
        output.close();
#endif

        turn_number = boost::lexical_cast<int>(doc.root_node.Attribute("turn_number"));

        // free current sitreps
        Empires().Lookup( m_empire_id )->ClearSitRep( );

        // Update data used XPatch and needs only elements common to universe and empire
        UpdateTurnData( doc );

        // Now decode sitreps
        // Empire sitreps need UI in order to generate text, since it needs string resources
        // generate textr for all sitreps
        for (Empire::SitRepItr sitrep_it = Empires().Lookup( m_empire_id )->SitRepBegin(); sitrep_it != Empires().Lookup( m_empire_id )->SitRepEnd(); ++sitrep_it) {

            SitRepEntry *pEntry = (*sitrep_it);
                
            // create string
             m_ui->GenerateSitRepText( pEntry );
        }
        Logger().debugStream() <<"HumanClientApp::HandleMessageImpl : Sitrep creation complete";

        m_ui->ScreenMap(); 
        m_ui->InitTurn( turn_number ); // init the new turn
        break;
    }

    case Message::TURN_PROGRESS: {
        GG::XMLDoc doc;
        int phase_id;
        int empire_id;
        std::string phase_str;
        std::stringstream stream(msg.GetText());

        doc.ReadDoc(stream);          

        phase_id = boost::lexical_cast<int>(doc.root_node.Child("phase_id").Attribute("value"));
        empire_id = boost::lexical_cast<int>(doc.root_node.Child("empire_id").Attribute("value"));

        // given IDs, build message
        if ( phase_id == Message::FLEET_MOVEMENT )
            phase_str = ClientUI::String("TURN_PROGRESS_PHASE_FLEET_MOVEMENT" );
        else if ( phase_id == Message::COMBAT )
            phase_str = ClientUI::String("TURN_PROGRESS_PHASE_COMBAT" );
        else if ( phase_id == Message::EMPIRE_PRODUCTION )
            phase_str = ClientUI::String("TURN_PROGRESS_PHASE_EMPIRE_GROWTH" );
        else if ( phase_id == Message::WAITING_FOR_PLAYERS )
            phase_str = ClientUI::String("TURN_PROGRESS_PHASE_WAITING");
        else if ( phase_id == Message::PROCESSING_ORDERS )
            phase_str = ClientUI::String("TURN_PROGRESS_PHASE_ORDERS");

        m_ui->UpdateTurnProgress( phase_str, empire_id );
        break;
    }

    case Message::COMBAT_START:
    case Message::COMBAT_ROUND_UPDATE:
    case Message::COMBAT_END:{
        m_ui->UpdateCombatTurnProgress(msg.GetText());
        break;
    }

    case Message::HUMAN_PLAYER_MSG: {
        GetUI()->GetMapWnd()->HandlePlayerChatMessage(msg.GetText());
        break;
    }

    case Message::PLAYER_ELIMINATED: {
        std::cout << "HumanClientApp::HandleMessageImpl : Message::PLAYER_ELIMINATED : m_empire_id=" << m_empire_id << " Empires().Lookup(m_empire_id)=" << Empires().Lookup(m_empire_id);
        if (Empires().Lookup(m_empire_id)->Name() == msg.GetText()) {
            // TODO: replace this with something better
            ClientUI::MessageBox("You are defeated.");
            EndGame();
        } else {
            // TODO: replace this with something better
            ClientUI::MessageBox(boost::io::str(boost::format(ClientUI::String("EMPIRE_DEFEATED")) % msg.GetText()));
        }
        break;
    }

    case Message::PLAYER_EXIT: {
        std::string message = boost::io::str(boost::format(ClientUI::String("PLAYER_DISCONNECTED")) % msg.GetText());
        ClientUI::MessageBox(message);
        break;
    }

    case Message::END_GAME: {
        if (msg.GetText() == "VICTORY") {
            // TODO: replace this with something better
            ClientUI::MessageBox("You are victorious.");
        } else {
            ClientUI::MessageBox(ClientUI::String("SERVER_GAME_END"));
        }
        EndGame();
        break;
    }

    default: {
        Logger().errorStream() << "HumanClientApp::HandleMessageImpl : Received unknown Message type code " << msg.Type();
        break;
    }
    }
}

void HumanClientApp::HandleServerDisconnectImpl()
{
    if (m_multiplayer_lobby_wnd) { // in MP lobby
        ClientUI::MessageBox(ClientUI::String("MPLOBBY_HOST_ABORTED_GAME"));
        m_multiplayer_lobby_wnd->Cancel();
    } else if (m_game_started) { // playing game
        ClientUI::MessageBox(ClientUI::String("SERVER_LOST"));
        EndGame();
    }
}

void HumanClientApp::EndOfMusicCallback()
{
    HumanClientApp* this_ptr = GetApp();
    if (!this_ptr->m_current_music)
        throw std::runtime_error("HumanClientApp::EndOfMusicCallback : End of a song was reached, but HumanClientApp::m_current_music == 0!");

    Mix_HaltMusic();
    Mix_FreeMusic(this_ptr->m_current_music);
    this_ptr->m_current_music = 0;
}

void HumanClientApp::EndOfSoundCallback(int channel)
{
    HumanClientApp* this_ptr = GetApp();
    std::map<std::string, Mix_Chunk*>::iterator it = this_ptr->m_sounds.find(this_ptr->m_channels[channel]);
    if (it == this_ptr->m_sounds.end()) {
        throw std::runtime_error("HumanClientApp::EndOfSoundCallback : End of a sound was reached, but there's no "
                                 "record of the filename associated with the channel that just stopped playing.");
    }
    std::string filename = it->first;
    this_ptr->m_channels[channel] = ""; 
    if (this_ptr->m_sounds_to_free.find(filename) != this_ptr->m_sounds_to_free.end())
        this_ptr->FreeSound(filename);
}


void HumanClientApp::StartTurn( )
{
  // setup GUI
  m_ui->ScreenProcessTurn( );

  // call base method
  ClientApp::StartTurn();
}

