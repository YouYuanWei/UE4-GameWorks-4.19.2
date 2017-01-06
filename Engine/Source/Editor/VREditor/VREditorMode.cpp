// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "VREditorMode.h"
#include "Modules/ModuleManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/EngineTypes.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Components/SpotLightComponent.h"
#include "GameFramework/WorldSettings.h"
#include "DrawDebugHelpers.h"
#include "VREditorUISystem.h"
#include "VIBaseTransformGizmo.h"
#include "ViewportWorldInteraction.h"
#include "VREditorWorldInteraction.h"
#include "VREditorAvatarActor.h"
#include "VREditorTeleporter.h"
#include "VREditorAutoScaler.h"

#include "CameraController.h"
#include "EngineGlobals.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "MotionControllerComponent.h"
#include "EngineAnalytics.h"
#include "IHeadMountedDisplay.h"
#include "Interfaces/IAnalyticsProvider.h"

#include "IViewportInteractionModule.h"
#include "VREditorMotionControllerInteractor.h"

#include "ViewportWorldInteractionManager.h"
#include "EditorWorldExtension.h"

#define LOCTEXT_NAMESPACE "VREditorMode"

namespace VREd
{
	static FAutoConsoleVariable UseMouseAsHandInForcedVRMode( TEXT( "VREd.UseMouseAsHandInForcedVRMode" ), 1, TEXT( "When in forced VR mode, enabling this setting uses the mouse cursor as a virtual hand instead of motion controllers" ) );
	static FAutoConsoleVariable ForceOculusMirrorMode( TEXT( "VREd.ForceOculusMirrorMode" ), 3, TEXT( "Which Oculus display mirroring mode to use (see MirrorWindowModeType in OculusRiftHMD.h)" ) );
	static FAutoConsoleVariable VRNearClipPlane(TEXT("VREd.VRNearClipPlane"), 1.0f, TEXT("The near clip plane to use for VR"));
}

// @todo vreditor: Hacky that we have to import these this way. (Plugin has them in a .cpp, not exported)

UVREditorMode::UVREditorMode() : 
	Super(),
	bWantsToExitMode( false ),
	ExitType( EVREditorExitType::Normal ),
	bIsFullyInitialized( false ),
	AppTimeModeEntered( FTimespan::Zero() ),
	AvatarActor( nullptr ),
   	FlashlightComponent( nullptr ),
	bIsFlashlightOn( false ),
	MotionControllerID( 0 ),	// @todo vreditor minor: We only support a single controller, and we assume the first controller are the motion controls
	UISystem( nullptr ),
	TeleporterSystem( nullptr ),
	AutoScalerSystem( nullptr ),
	WorldInteraction( nullptr ),
	LeftHandInteractor( nullptr ),
	RightHandInteractor( nullptr ),
	bFirstTick( true ),
	bIsActive( false )
{
}


UVREditorMode::~UVREditorMode()
{
	Shutdown();
}


void UVREditorMode::Init()
{
	// @todo vreditor urgent: Turn on global editor hacks for VR Editor mode
	GEnableVREditorHacks = true;

	bIsFullyInitialized = false;
	bWantsToExitMode = false;

	AppTimeModeEntered = FTimespan::FromSeconds( FApp::GetCurrentTime() );

	// Take note of VREditor activation
	if( FEngineAnalytics::IsAvailable() )
	{
		FEngineAnalytics::GetProvider().RecordEvent( TEXT( "Editor.Usage.EnterVREditorMode" ) );
	}

	// Setting up colors
	Colors.SetNumZeroed( (int32)EColors::TotalCount );
	{
		Colors[ (int32)EColors::DefaultColor ] = FLinearColor::Red;	
		Colors[ (int32)EColors::SelectionColor ] = FLinearColor::Green;
		Colors[ (int32)EColors::TeleportColor ] = FLinearColor( 1.0f, 0.0f, 0.75f, 1.0f );
		Colors[ (int32)EColors::WorldDraggingColor_OneHand ] = FLinearColor::Yellow;
		Colors[ (int32)EColors::WorldDraggingColor_TwoHands ] = FLinearColor( 0.05f, 0.05f, 0.4f, 1.0f );
		Colors[ (int32)EColors::RedGizmoColor ] = FLinearColor( 0.4f, 0.05f, 0.05f, 1.0f );
		Colors[ (int32)EColors::GreenGizmoColor ] = FLinearColor( 0.05f, 0.4f, 0.05f, 1.0f );
		Colors[ (int32)EColors::BlueGizmoColor ] = FLinearColor( 0.05f, 0.05f, 0.4f, 1.0f );
		Colors[ (int32)EColors::WhiteGizmoColor ] = FLinearColor( 0.7f, 0.7f, 0.7f, 1.0f );
		Colors[ (int32)EColors::HoverGizmoColor ] = FLinearColor( FLinearColor::Yellow );
		Colors[ (int32)EColors::DraggingGizmoColor ] = FLinearColor( FLinearColor::White );
		Colors[ (int32)EColors::UISelectionBarColor ] = FLinearColor( 0.025f, 0.025f, 0.025f, 1.0f );
		Colors[ (int32)EColors::UISelectionBarHoverColor ] = FLinearColor( 0.1f, 0.1f, 0.1f, 1.0f );
		Colors[ (int32)EColors::UICloseButtonColor ] = FLinearColor( 0.1f, 0.1f, 0.1f, 1.0f );
		Colors[ (int32)EColors::UICloseButtonHoverColor ] = FLinearColor( 1.0f, 1.0f, 1.0f, 1.0f );
	}

	{
		TSharedPtr<FEditorWorldExtensionCollection> Collection = GEditor->GetEditorWorldExtensionsManager()->GetEditorWorldExtensions( GetWorld() );
		WorldInteraction = Cast<UViewportWorldInteraction>( Collection->FindExtension( UViewportWorldInteraction::StaticClass() ) );
		check( WorldInteraction != nullptr );
	}

	bIsFullyInitialized = true;
}

void UVREditorMode::Shutdown()
{
	bIsFullyInitialized = false;
	
	AvatarActor = nullptr;
	FlashlightComponent = nullptr;
	UISystem = nullptr;
	TeleporterSystem = nullptr;
	AutoScalerSystem = nullptr;
	WorldInteraction = nullptr;
	LeftHandInteractor = nullptr;
	RightHandInteractor = nullptr;

	// @todo vreditor urgent: Disable global editor hacks for VR Editor mode
	GEnableVREditorHacks = false;
}

void UVREditorMode::Enter(const bool bReenteringVREditing)
{
	bWantsToExitMode = false;
	ExitType = EVREditorExitType::Normal;

	{
		WorldInteraction->OnPreWorldInteractionTick().AddUObject( this, &UVREditorMode::PreTick );
		WorldInteraction->OnPostWorldInteractionTick().AddUObject( this, &UVREditorMode::PostTick );
	}

	// @todo vreditor: We need to make sure the user can never switch to orthographic mode, or activate settings that
	// would disrupt the user's ability to view the VR scene.

	// @todo vreditor: Don't bother drawing toolbars in VR, or other things that won't matter in VR

	{
		const TSharedRef< ILevelEditor >& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor").GetFirstLevelEditor().ToSharedRef();

		bool bSummonNewWindow = true;

		// Do we have an active perspective viewport that is valid for VR?  If so, go ahead and use that.
		TSharedPtr<SLevelViewport> ExistingActiveLevelViewport;
		{
			TSharedPtr<ILevelViewport> ActiveLevelViewport = LevelEditor->GetActiveViewportInterface();
			if(ActiveLevelViewport.IsValid())
			{
				ExistingActiveLevelViewport = StaticCastSharedRef< SLevelViewport >(ActiveLevelViewport->AsWidget());

				// Use the currently active window instead
				bSummonNewWindow = false;
			}
		}

		TSharedPtr< SLevelViewport > VREditorLevelViewport;
		if(bSummonNewWindow)
		{
			// @todo vreditor: The resolution we set here doesn't matter, as HMDs will draw at their native resolution
			// no matter what.  We should probably allow the window to be freely resizable by the user
			// @todo vreditor: Should save and restore window position and size settings
			FVector2D WindowSize;
			{
				IHeadMountedDisplay::MonitorInfo HMDMonitorInfo;
				if(bActuallyUsingVR && GEngine->HMDDevice->GetHMDMonitorInfo(HMDMonitorInfo))
				{
					WindowSize = FVector2D(HMDMonitorInfo.ResolutionX, HMDMonitorInfo.ResolutionY);
				}
				else
				{
					// @todo vreditor: Hard-coded failsafe window size
					WindowSize = FVector2D(1920.0f, 1080.0f);
				}
			}

			// @todo vreditor: Use SLevelEditor::GetTableTitle() for the VR window title (needs dynamic update)
			const FText VREditorWindowTitle = NSLOCTEXT("VREditor", "VRWindowTitle", "Unreal Editor VR");

			TSharedRef< SWindow > VREditorWindow = SNew(SWindow)
				.Title(VREditorWindowTitle)
				.ClientSize(WindowSize)
				.AutoCenter(EAutoCenter::PreferredWorkArea)
				.UseOSWindowBorder(true)	// @todo vreditor: Allow window to be freely resized?  Shouldn't really hurt anything.  We should save position/size too.
				.SizingRule(ESizingRule::UserSized);
			this->VREditorWindowWeakPtr = VREditorWindow;

			VREditorLevelViewport =
				SNew(SLevelViewport)
				.ViewportType(LVT_Perspective) // Perspective
				.Realtime(true)
				//				.ParentLayout( AsShared() )	// @todo vreditor: We don't have one and we probably don't need one, right?  Make sure a null parent layout is handled properly everywhere.
				.ParentLevelEditor(LevelEditor)
				//				.ConfigKey( BottomLeftKey )	// @todo vreditor: This is for saving/loading layout.  We would need this in order to remember viewport settings like show flags, etc.
				.IsEnabled(FSlateApplication::Get().GetNormalExecutionAttribute());

			// Allow the editor to keep track of this editor viewport.  Because it's not inside of a normal tab, 
			// we need to explicitly tell the level editor about it
			LevelEditor->AddStandaloneLevelViewport(VREditorLevelViewport.ToSharedRef());

			VREditorWindow->SetContent(VREditorLevelViewport.ToSharedRef());

			// NOTE: We're intentionally not adding this window natively parented to the main frame window, because we don't want it
			// to minimize/restore when the main frame is minimized/restored
			FSlateApplication::Get().AddWindow(VREditorWindow);

			VREditorWindow->SetOnWindowClosed(FOnWindowClosed::CreateUObject(this, &UVREditorMode::OnVREditorWindowClosed));

			VREditorWindow->BringToFront();	// @todo vreditor: Not sure if this is needed, especially if we decide the window should be hidden (copied this from PIE code)
		}
		else
		{
			VREditorLevelViewport = ExistingActiveLevelViewport;

			if(bActuallyUsingVR)
			{
				// Switch to immersive mode
				const bool bWantImmersive = true;
				const bool bAllowAnimation = false;
				ExistingActiveLevelViewport->MakeImmersive(bWantImmersive, bAllowAnimation);
			}
		}

		this->VREditorLevelViewportWeakPtr = VREditorLevelViewport;

		{
			FLevelEditorViewportClient& VRViewportClient = VREditorLevelViewport->GetLevelViewportClient();
			FEditorViewportClient& VREditorViewportClient = VRViewportClient;

			// Make sure we are in perspective mode
			// @todo vreditor: We should never allow ortho switching while in VR
			SavedEditorState.ViewportType = VREditorViewportClient.GetViewportType();
			VREditorViewportClient.SetViewportType(LVT_Perspective);

			// Set the initial camera location
			// @todo vreditor: This should instead be calculated using the currently active perspective camera's
			// location and orientation, compensating for the current HMD offset from the tracking space origin.
			// Perhaps, we also want to teleport the original viewport's camera back when we exit this mode, too!
			// @todo vreditor: Should save and restore camera position and any other settings we change (viewport type, pitch locking, etc.)
			SavedEditorState.ViewLocation = VRViewportClient.GetViewLocation();
			SavedEditorState.ViewRotation = VRViewportClient.GetViewRotation();

			// Don't allow the tracking space to pitch up or down.  People hate that in VR.
			// @todo vreditor: This doesn't seem to prevent people from pitching the camera with RMB drag
			SavedEditorState.bLockedPitch = VRViewportClient.GetCameraController()->GetConfig().bLockedPitch;
			if(bActuallyUsingVR)
			{
				VRViewportClient.GetCameraController()->AccessConfig().bLockedPitch = true;
			}

			// Set "game mode" to be enabled, to get better performance.  Also hit proxies won't work in VR, anyway
			SavedEditorState.bGameView = VREditorViewportClient.IsInGameView();
			VREditorViewportClient.SetGameView(true);

			SavedEditorState.bRealTime = VREditorViewportClient.IsRealtime();
			VREditorViewportClient.SetRealtime(true);

			SavedEditorState.ShowFlags = VREditorViewportClient.EngineShowFlags;

			// Never show the traditional Unreal transform widget.  It doesn't work in VR because we don't have hit proxies.
			VREditorViewportClient.EngineShowFlags.SetModeWidgets(false);

			// Make sure the mode widgets don't come back when users click on things
			VRViewportClient.bAlwaysShowModeWidgetAfterSelectionChanges = false;

			// Force tiny near clip plane distance, because user can scale themselves to be very small.
			SavedEditorState.NearClipPlane = GNearClippingPlane;
			GNearClippingPlane = GetDefaultVRNearClipPlane();	

			SavedEditorState.bOnScreenMessages = GAreScreenMessagesEnabled;
			GAreScreenMessagesEnabled = false;

			// Save the world to meters scale
			SavedEditorState.WorldToMetersScale = VRViewportClient.GetWorld()->GetWorldSettings()->WorldToMeters;

			if(bActuallyUsingVR)
			{
				SavedEditorState.TrackingOrigin = GEngine->HMDDevice->GetTrackingOrigin();
				GEngine->HMDDevice->SetTrackingOrigin(EHMDTrackingOrigin::Floor);
			}

			// Make the new viewport the active level editing viewport right away
			GCurrentLevelEditingViewportClient = &VRViewportClient;

			// Enable selection outline right away
			VREditorViewportClient.EngineShowFlags.SetSelection( true );
			VREditorViewportClient.EngineShowFlags.SetSelectionOutline( true );

			// Change viewport settings to more VR-friendly sequencer settings
			SavedEditorState.bCinematicPreviewViewport = VRViewportClient.AllowsCinematicPreview();
			VRViewportClient.SetAllowCinematicPreview(false);
			// Need to force fading and color scaling off in case we enter VR editing mode with a sequence open
			VRViewportClient.bEnableFading = false;
			VRViewportClient.bEnableColorScaling = false;
			VRViewportClient.Invalidate(true);
		}

		VREditorLevelViewport->EnableStereoRendering( bActuallyUsingVR );
		VREditorLevelViewport->SetRenderDirectlyToWindow( bActuallyUsingVR );


		if( bActuallyUsingVR )
		{
			if( !( GEngine->HMDDevice->IsStereoEnabled() ) || bReenteringVREditing )
			{
				GEngine->HMDDevice->EnableStereo( true );
			}
		}

		if( bActuallyUsingVR )
		{
			// Tell Slate to require a larger pixel distance threshold before the drag starts.  This is important for things 
			// like Content Browser drag and drop.
			SavedEditorState.DragTriggerDistance = FSlateApplication::Get().GetDragTriggerDistance();
			FSlateApplication::Get().SetDragTriggerDistance( 100.0f );	// @todo vreditor tweak

			// When actually in VR, make sure the transform gizmo is big!
			SavedEditorState.TransformGizmoScale = WorldInteraction->GetTransformGizmoScale();
			WorldInteraction->SetTransformGizmoScale( 1.0f );
		}
	}

	// Setup sub systems
	{
		// Setup world interaction
		TSharedPtr<FEditorViewportClient> ViewportClient = VREditorLevelViewportWeakPtr.Pin()->GetViewportClient();

		// We're not expecting anyone to have already set a default viewport client
		check( WorldInteraction->GetDefaultOptionalViewportClient() == nullptr );
		WorldInteraction->SetDefaultOptionalViewportClient( ViewportClient );

		// We need input preprocessing for VR so that we can receive motion controller input without any viewports having 
		// to be focused.  This is mainly because Slate UI injected into the 3D world can cause focus to be lost unexpectedly,
		// but we need the user to still be able to interact with UI.
		WorldInteraction->SetUseInputProcessor( true );

		// Motion controllers
		{
			LeftHandInteractor = NewObject<UVREditorMotionControllerInteractor>();
			LeftHandInteractor->SetControllerHandSide( EControllerHand::Left );
			LeftHandInteractor->Init( this );
			WorldInteraction->AddInteractor( LeftHandInteractor );

			RightHandInteractor = NewObject<UVREditorMotionControllerInteractor>();
			RightHandInteractor->SetControllerHandSide( EControllerHand::Right );
			RightHandInteractor->Init( this );
			WorldInteraction->AddInteractor( RightHandInteractor );

			WorldInteraction->PairInteractors( LeftHandInteractor, RightHandInteractor );
		}

		if( !bActuallyUsingVR )
		{
			// When not actually using VR devices, we want an interactor for our mouse cursor!
			WorldInteraction->AddMouseCursorInteractor();
		}

		// Setup the UI system
		UISystem = NewObject<UVREditorUISystem>();
		UISystem->SetOwner( this );
		UISystem->Init();

		VRWorldInteractionExtension = NewObject<UVREditorWorldInteraction>();
		VRWorldInteractionExtension->Init( this, WorldInteraction );

		// Setup teleporter
		TeleporterSystem = NewObject<UVREditorTeleporter>();
		TeleporterSystem->Init( this );

		// Setup autoscaler
		AutoScalerSystem = NewObject<UVREditorAutoScaler>();
		AutoScalerSystem->Init( this );
	}

	if(AvatarActor == nullptr)
	{
		SpawnAvatarMeshActor();
	}

	/** This will make sure this is not ticking after the editor has been closed. */
	GEditor->OnEditorClose().AddUObject( this, &UVREditorMode::OnEditorClosed );

	bFirstTick = true;
	bIsActive = true;
}

void UVREditorMode::Exit(const bool bHMDShouldExitStereo)
{
	{
		//Destroy the avatar
		{
			UViewportWorldInteraction::DestroyTransientActor( GetWorld(), AvatarActor );
			AvatarActor = nullptr;
			FlashlightComponent = nullptr;
		}

		{
			if(GEngine->HMDDevice.IsValid() && bHMDShouldExitStereo)
			{
				// @todo vreditor switch: We don't want to do this if a VR PIE session is somehow active.  Is that even possible while immersive?
				GEngine->HMDDevice->EnableStereo( false );
			}

			if(bActuallyUsingVR)
			{
				// Restore Slate drag trigger distance
				FSlateApplication::Get().SetDragTriggerDistance( SavedEditorState.DragTriggerDistance );

				// Restore gizmo size
				WorldInteraction->SetTransformGizmoScale( SavedEditorState.TransformGizmoScale );
			}

			TSharedPtr<SLevelViewport> VREditorLevelViewport( VREditorLevelViewportWeakPtr.Pin() );
			if(VREditorLevelViewport.IsValid())
			{
				VREditorLevelViewport->EnableStereoRendering( false );
				VREditorLevelViewport->SetRenderDirectlyToWindow( false );
				{
					FLevelEditorViewportClient& VRViewportClient = VREditorLevelViewport->GetLevelViewportClient();
					FEditorViewportClient& VREditorViewportClient = VRViewportClient;

					// Restore settings that we changed on the viewport
					VREditorViewportClient.SetViewportType( SavedEditorState.ViewportType );
					VRViewportClient.GetCameraController()->AccessConfig().bLockedPitch = SavedEditorState.bLockedPitch;
					VRViewportClient.bAlwaysShowModeWidgetAfterSelectionChanges = SavedEditorState.bAlwaysShowModeWidgetAfterSelectionChanges;
					VRViewportClient.EngineShowFlags = SavedEditorState.ShowFlags;
					VRViewportClient.SetGameView( SavedEditorState.bGameView );
					VRViewportClient.SetAllowCinematicPreview(SavedEditorState.bCinematicPreviewViewport);
					VRViewportClient.bEnableFading = true;
					VRViewportClient.bEnableColorScaling = true;
					VRViewportClient.Invalidate(true);

					if(bActuallyUsingVR)
					{
						VRViewportClient.SetViewLocation( GetHeadTransform().GetLocation() );

						FRotator HeadRotationNoRoll = GetHeadTransform().GetRotation().Rotator();
						HeadRotationNoRoll.Roll = 0.0f;
						VRViewportClient.SetViewRotation( HeadRotationNoRoll ); // Use SavedEditorState.ViewRotation to go back to start rot
					}

					VRViewportClient.SetRealtime( SavedEditorState.bRealTime );

					GNearClippingPlane = SavedEditorState.NearClipPlane;
					GAreScreenMessagesEnabled = SavedEditorState.bOnScreenMessages;

					if(bActuallyUsingVR)
					{
						GEngine->HMDDevice->SetTrackingOrigin( SavedEditorState.TrackingOrigin );
					}

					// Set the world to meters back to the saved one when entering the mode
					{
						VRViewportClient.GetWorld()->GetWorldSettings()->WorldToMeters = SavedEditorState.WorldToMetersScale;
						ENGINE_API extern float GNewWorldToMetersScale;
						GNewWorldToMetersScale = 0.0f;
						OnVREditingModeExit_Handler.ExecuteIfBound();
					}
				}

				if(bActuallyUsingVR)
				{
					// Leave immersive mode
					const bool bWantImmersive = false;
					const bool bAllowAnimation = false;
					VREditorLevelViewport->MakeImmersive( bWantImmersive, bAllowAnimation );
				}

				VREditorLevelViewportWeakPtr.Reset();
			}

			// Kill the VR editor window
			TSharedPtr<SWindow> VREditorWindow( VREditorWindowWeakPtr.Pin() );
			if(VREditorWindow.IsValid())
			{
				VREditorWindow->RequestDestroyWindow();
				VREditorWindow.Reset();
			}
		}

		// Kill the VR editor window
		TSharedPtr<SWindow> VREditorWindow( VREditorWindowWeakPtr.Pin() );
		if( VREditorWindow.IsValid() )
		{
			VREditorWindow->RequestDestroyWindow();
			VREditorWindow.Reset();
		}
	}
	//Destroy the avatar
	{
		UViewportWorldInteraction::DestroyTransientActor( GetWorld(), AvatarActor );
		AvatarActor = nullptr;
		FlashlightComponent = nullptr;
	}

	// Kill subsystems
	if( UISystem != nullptr )
	{
		UISystem->Shutdown();
		UISystem->MarkPendingKill();
		UISystem = nullptr;
	}

	if( VRWorldInteractionExtension != nullptr )
	{
		VRWorldInteractionExtension->MarkPendingKill();
		VRWorldInteractionExtension = nullptr;
	}

	if( TeleporterSystem != nullptr )
	{
		TeleporterSystem->Shutdown();
		TeleporterSystem->MarkPendingKill();
		TeleporterSystem = nullptr;
	}

	if( AutoScalerSystem != nullptr )
	{
		AutoScalerSystem->Shutdown();
		AutoScalerSystem->MarkPendingKill();
		AutoScalerSystem = nullptr;
	}

	if( WorldInteraction != nullptr )
	{
		WorldInteraction->OnHandleKeyInput().RemoveAll( this );
		WorldInteraction->OnPreWorldInteractionTick().RemoveAll( this );
		WorldInteraction->OnPostWorldInteractionTick().RemoveAll( this );

		WorldInteraction->RemoveInteractor( LeftHandInteractor );
		LeftHandInteractor->MarkPendingKill();
		LeftHandInteractor->Shutdown();
		LeftHandInteractor = nullptr;

		WorldInteraction->RemoveInteractor( RightHandInteractor );
		RightHandInteractor->Shutdown();
		RightHandInteractor->MarkPendingKill();
		RightHandInteractor = nullptr;
		
		if( !bActuallyUsingVR )
		{
			WorldInteraction->ReleaseMouseCursorInteractor();
		}

		WorldInteraction->SetDefaultOptionalViewportClient( nullptr );
	}

	GEditor->OnEditorClose().RemoveAll( this );

	bWantsToExitMode = false;
	bIsActive = false;
	bFirstTick = false;
}

void UVREditorMode::OnEditorClosed()
{
	if( bIsActive )
	{
		Exit( false );
		Shutdown();
	}
}

void UVREditorMode::StartExitingVRMode( const EVREditorExitType InExitType /*= EVREditorExitType::To_Editor */ )
{
	ExitType = InExitType;
	bWantsToExitMode = true;
}

void UVREditorMode::SpawnAvatarMeshActor()
{
	// Setup our avatar
	if( AvatarActor == nullptr )
	{
		{
			const bool bWithSceneComponent = true;
			AvatarActor = UViewportWorldInteraction::SpawnTransientSceneActor<AVREditorAvatarActor>( GetWorld(), TEXT( "AvatarActor" ), bWithSceneComponent );
			AvatarActor->Init( this );
		}

		//@todo VREditor: Hardcoded interactors
		LeftHandInteractor->SetupComponent( AvatarActor );
		RightHandInteractor->SetupComponent( AvatarActor );
	}
}


void UVREditorMode::OnVREditorWindowClosed( const TSharedRef<SWindow>& ClosedWindow )
{
	StartExitingVRMode();
}

void UVREditorMode::PreTick( const float DeltaTime )
{
	if( !bIsFullyInitialized || !bIsActive || bWantsToExitMode )
	{
		return;
	}

	//Setting the initial position and rotation based on the editor viewport when going into VR mode
	if( bFirstTick && bActuallyUsingVR )
	{
		const FTransform RoomToWorld = GetRoomTransform();
		const FTransform WorldToRoom = RoomToWorld.Inverse();
		FTransform ViewportToWorld = FTransform( SavedEditorState.ViewRotation, SavedEditorState.ViewLocation );
		FTransform ViewportToRoom = ( ViewportToWorld * WorldToRoom );

		FTransform ViewportToRoomYaw = ViewportToRoom;
		ViewportToRoomYaw.SetRotation( FQuat( FRotator( 0.0f, ViewportToRoomYaw.GetRotation().Rotator().Yaw, 0.0f ) ) );

		FTransform HeadToRoomYaw = GetRoomSpaceHeadTransform();
		HeadToRoomYaw.SetRotation( FQuat( FRotator( 0.0f, HeadToRoomYaw.GetRotation().Rotator().Yaw, 0.0f ) ) );

		FTransform RoomToWorldYaw = RoomToWorld;
		RoomToWorldYaw.SetRotation( FQuat( FRotator( 0.0f, RoomToWorldYaw.GetRotation().Rotator().Yaw, 0.0f ) ) );

		FTransform ResultToWorld = ( HeadToRoomYaw.Inverse() * ViewportToRoomYaw ) * RoomToWorldYaw;
		SetRoomTransform( ResultToWorld );
	}

}

void UVREditorMode::PostTick( float DeltaTime )
{
	if( !bIsFullyInitialized || !bIsActive || bWantsToExitMode || !VREditorLevelViewportWeakPtr.IsValid() )
	{
		return;
	}

	TickHandle.Broadcast( DeltaTime );
	UISystem->Tick( GetLevelViewportPossessedForVR().GetViewportClient().Get(), DeltaTime );

	// Update avatar meshes
	{
		// Move our avatar mesh along with the room.  We need our hand components to remain the same coordinate space as the 
		AvatarActor->SetActorTransform( GetRoomTransform() );
		AvatarActor->TickManually( DeltaTime );
	}

	// Updating the scale and intensity of the flashlight according to the world scale
	if (FlashlightComponent)
	{
		float CurrentFalloffExponent = FlashlightComponent->LightFalloffExponent;
		//@todo vreditor tweak
		float UpdatedFalloffExponent = FMath::Clamp(CurrentFalloffExponent / GetWorldScaleFactor(), 2.0f, 16.0f);
		FlashlightComponent->SetLightFalloffExponent(UpdatedFalloffExponent);
	}

	StopOldHapticEffects();

	bFirstTick = false;
}

bool UVREditorMode::InputKey( FEditorViewportClient* InViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event )
{
	bool bHandled = false;
	if( Key == EKeys::Escape )
	{
		// User hit escape, so bail out of VR mode
		StartExitingVRMode();
		bHandled = true;
	}

	return bHandled;
}


/************************************************************************/
/* IVREditorMode interface                                              */
/************************************************************************/

AActor* UVREditorMode::GetAvatarMeshActor()
{
	return AvatarActor;
}

UWorld* UVREditorMode::GetWorld() const
{
	UWorld* ResultedWorld = nullptr;
	if( WorldInteraction )
	{
		ResultedWorld = WorldInteraction->GetWorld();
	}
	else
	{
		ResultedWorld = UEditorWorldExtension::GetWorld();
	}

	return ResultedWorld;
}

FTransform UVREditorMode::GetRoomTransform() const
{
	return WorldInteraction->GetRoomTransform();
}

void UVREditorMode::SetRoomTransform( const FTransform& NewRoomTransform )
{
	WorldInteraction->SetRoomTransform( NewRoomTransform );
}

FTransform UVREditorMode::GetRoomSpaceHeadTransform() const
{
	return WorldInteraction->GetRoomSpaceHeadTransform();
}

FTransform UVREditorMode::GetHeadTransform() const
{
	return WorldInteraction->GetHeadTransform();
}

const UViewportWorldInteraction& UVREditorMode::GetWorldInteraction() const
{
	return *WorldInteraction;
}

UViewportWorldInteraction& UVREditorMode::GetWorldInteraction()
{
	return *WorldInteraction;
}

bool UVREditorMode::IsFullyInitialized() const
{
	return bIsFullyInitialized;
}

bool UVREditorMode::IsActive() const
{
	return bIsActive;
}

bool UVREditorMode::IsShowingRadialMenu(const UVREditorInteractor* Interactor) const
{
	return UISystem->IsShowingRadialMenu(Interactor);
}

const SLevelViewport& UVREditorMode::GetLevelViewportPossessedForVR() const
{
	return *VREditorLevelViewportWeakPtr.Pin();
}

SLevelViewport& UVREditorMode::GetLevelViewportPossessedForVR()
{
	return *VREditorLevelViewportWeakPtr.Pin();
}


float UVREditorMode::GetWorldScaleFactor() const
{
	return WorldInteraction->GetWorldScaleFactor();
}


void UVREditorMode::CleanUpActorsBeforeMapChangeOrSimulate()
{
	if ( WorldInteraction != nullptr )
	{
		// NOTE: This will be called even when this mode is not currently active!
		UViewportWorldInteraction::DestroyTransientActor( GetWorld(), AvatarActor );
		AvatarActor = nullptr;
		FlashlightComponent = nullptr;

		if ( UISystem != nullptr )
		{
			UISystem->CleanUpActorsBeforeMapChangeOrSimulate();
		}

		WorldInteraction->Shutdown();
	}
}

void UVREditorMode::ToggleFlashlight( UVREditorInteractor* Interactor )
{
	UVREditorMotionControllerInteractor* MotionControllerInteractor = Cast<UVREditorMotionControllerInteractor>( Interactor );
	if ( MotionControllerInteractor )
	{
		if ( FlashlightComponent == nullptr )
		{
			FlashlightComponent = NewObject<USpotLightComponent>( AvatarActor );
			AvatarActor->AddOwnedComponent( FlashlightComponent );
			FlashlightComponent->RegisterComponent();
			FlashlightComponent->SetMobility( EComponentMobility::Movable );
			FlashlightComponent->SetCastShadows( false );
			FlashlightComponent->bUseInverseSquaredFalloff = false;
			//@todo vreditor tweak
			FlashlightComponent->SetLightFalloffExponent( 8.0f );
			FlashlightComponent->SetIntensity( 20.0f );
			FlashlightComponent->SetOuterConeAngle( 25.0f );
			FlashlightComponent->SetInnerConeAngle( 25.0f );

		}

		const FAttachmentTransformRules AttachmentTransformRules = FAttachmentTransformRules( EAttachmentRule::KeepRelative, true );
		FlashlightComponent->AttachToComponent( MotionControllerInteractor->GetMotionControllerComponent(), AttachmentTransformRules );
		bIsFlashlightOn = !bIsFlashlightOn;
		FlashlightComponent->SetVisibility( bIsFlashlightOn );
	}
}

void UVREditorMode::CycleTransformGizmoHandleType()
{
	EGizmoHandleTypes NewGizmoType = (EGizmoHandleTypes)( (uint8)WorldInteraction->GetCurrentGizmoType() + 1 );
	
	if( NewGizmoType > EGizmoHandleTypes::Scale )
	{
		NewGizmoType = EGizmoHandleTypes::All;
	}

	WorldInteraction->SetGizmoHandleType( NewGizmoType );
}

EHMDDeviceType::Type UVREditorMode::GetHMDDeviceType() const
{
	return GEngine->HMDDevice.IsValid() ? GEngine->HMDDevice->GetHMDDeviceType() : EHMDDeviceType::DT_SteamVR;
}

FLinearColor UVREditorMode::GetColor( const EColors Color ) const
{
	return Colors[ (int32)Color ];
}

float UVREditorMode::GetDefaultVRNearClipPlane() const 
{
	return VREd::VRNearClipPlane->GetFloat();
}

void UVREditorMode::RefreshVREditorSequencer(class ISequencer* InCurrentSequencer)
{
	CurrentSequencer = InCurrentSequencer;
	// Tell the VR Editor UI system to refresh the Sequencer UI
	GetUISystem().UpdateSequencerUI();
}

class ISequencer* UVREditorMode::GetCurrentSequencer()
{
	return CurrentSequencer;
}

bool UVREditorMode::IsHandAimingTowardsCapsule(UViewportInteractor* Interactor, const FTransform& CapsuleTransform, FVector CapsuleStart, const FVector CapsuleEnd, const float CapsuleRadius, const float MinDistanceToCapsule, const FVector CapsuleFrontDirection, const float MinDotForAimingAtCapsule) const
{
	bool bIsAimingTowards = false;
	const float WorldScaleFactor = GetWorldScaleFactor();

	FVector LaserPointerStart, LaserPointerEnd;
	if( Interactor->GetLaserPointer( /* Out */ LaserPointerStart, /* Out */ LaserPointerEnd ) )
	{
		const FVector LaserPointerStartInCapsuleSpace = CapsuleTransform.InverseTransformPosition( LaserPointerStart );
		const FVector LaserPointerEndInCapsuleSpace = CapsuleTransform.InverseTransformPosition( LaserPointerEnd );

		FVector ClosestPointOnLaserPointer, ClosestPointOnUICapsule;
		FMath::SegmentDistToSegment(
			LaserPointerStartInCapsuleSpace, LaserPointerEndInCapsuleSpace,
			CapsuleStart, CapsuleEnd,
			/* Out */ ClosestPointOnLaserPointer,
			/* Out */ ClosestPointOnUICapsule );

		const bool bIsClosestPointInsideCapsule = ( ClosestPointOnLaserPointer - ClosestPointOnUICapsule ).Size() <= CapsuleRadius;

		const FVector TowardLaserPointerVector = ( ClosestPointOnLaserPointer - ClosestPointOnUICapsule ).GetSafeNormal();

		// Apply capsule radius
		ClosestPointOnUICapsule += TowardLaserPointerVector * CapsuleRadius;

		if( false )	// @todo vreditor debug
		{
			const float RenderCapsuleLength = ( CapsuleEnd - CapsuleStart ).Size() + CapsuleRadius * 2.0f;
			// @todo vreditor:  This capsule draws with the wrong orientation
			if( false )
			{
				DrawDebugCapsule( GetWorld(), CapsuleTransform.TransformPosition( CapsuleStart + ( CapsuleEnd - CapsuleStart ) * 0.5f ), RenderCapsuleLength * 0.5f, CapsuleRadius, CapsuleTransform.GetRotation() * FRotator( 90.0f, 0, 0 ).Quaternion(), FColor::Green, false, 0.0f );
			}
			DrawDebugLine( GetWorld(), CapsuleTransform.TransformPosition( ClosestPointOnLaserPointer ), CapsuleTransform.TransformPosition( ClosestPointOnUICapsule ), FColor::Green, false, 0.0f );
			DrawDebugSphere( GetWorld(), CapsuleTransform.TransformPosition( ClosestPointOnLaserPointer ), 1.5f * WorldScaleFactor, 32, FColor::Red, false, 0.0f );
			DrawDebugSphere( GetWorld(), CapsuleTransform.TransformPosition( ClosestPointOnUICapsule ), 1.5f * WorldScaleFactor, 32, FColor::Green, false, 0.0f );
		}

		// If we're really close to the capsule
		if( bIsClosestPointInsideCapsule ||
			( ClosestPointOnUICapsule - ClosestPointOnLaserPointer ).Size() <= MinDistanceToCapsule )
		{
			const FVector LaserPointerDirectionInCapsuleSpace = ( LaserPointerEndInCapsuleSpace - LaserPointerStartInCapsuleSpace ).GetSafeNormal();

			if( false )	// @todo vreditor debug
			{
				DrawDebugLine( GetWorld(), CapsuleTransform.TransformPosition( FVector::ZeroVector ), CapsuleTransform.TransformPosition( CapsuleFrontDirection * 5.0f ), FColor::Yellow, false, 0.0f );
				DrawDebugLine( GetWorld(), CapsuleTransform.TransformPosition( FVector::ZeroVector ), CapsuleTransform.TransformPosition( -LaserPointerDirectionInCapsuleSpace * 5.0f ), FColor::Purple, false, 0.0f );
			}

			const float Dot = FVector::DotProduct( CapsuleFrontDirection, -LaserPointerDirectionInCapsuleSpace );
			if( Dot >= MinDotForAimingAtCapsule )
			{
				bIsAimingTowards = true;
			}
		}
	}

	return bIsAimingTowards;
}

UVREditorInteractor* UVREditorMode::GetHandInteractor( const EControllerHand ControllerHand ) const 
{
	UVREditorInteractor* ResultInteractor = ControllerHand == EControllerHand::Left ? LeftHandInteractor : RightHandInteractor;
	check( ResultInteractor != nullptr );
	return ResultInteractor;
}

void UVREditorMode::StopOldHapticEffects()
{
	LeftHandInteractor->StopOldHapticEffects();
	RightHandInteractor->StopOldHapticEffects();
}

void UVREditorMode::SnapSelectedActorsToGround()
{
	VRWorldInteractionExtension->SnapSelectedActorsToGround();
}

const UVREditorMode::FSavedEditorState& UVREditorMode::GetSavedEditorState() const
{
	return SavedEditorState;
}

void UVREditorMode::SaveSequencerSettings(bool bInKeyAllEnabled, EAutoKeyMode InAutoKeyMode)
{
	SavedEditorState.bKeyAllEnabled = bInKeyAllEnabled;
	SavedEditorState.AutoKeyMode = InAutoKeyMode;
}

#undef LOCTEXT_NAMESPACE
