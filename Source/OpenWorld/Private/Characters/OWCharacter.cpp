// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/OWCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "NiagaraFunctionLibrary.h"
#include "Weapons/MeleeWeapon.h"

AOWCharacter::AOWCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// Pawn
	bUseControllerRotationYaw = bUseControllerRotationPitch = bUseControllerRotationRoll = false;

	// Make the mesh world dynamic so it can be hit by weapon
	GetMesh()->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	GetMesh()->SetGenerateOverlapEvents(true);
	GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);

	// Movement
	/* General */
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->MaxAcceleration = 500.f;
	GetCharacterMovement()->RotationRate = FRotator(0.f, 360.f, 0.f);
	/* Walking */
	GetCharacterMovement()->GroundFriction = 1.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 100.f;
	/* Crouching */
	GetCharacterMovement()->NavAgentProps.bCanCrouch = true;

	// Kick hitbox
	KickHitbox = CreateDefaultSubobject<USphereComponent>(TEXT("Kick Hitbox"));
	KickHitbox->SetupAttachment(GetMesh(), TEXT("foot_r"));	// * Make sure we have "foot_r" socket
	KickHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision); /* Set it to query only later */
	KickHitbox->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	KickHitbox->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Overlap);

	// ...
	DefaultInitializer();
}

void AOWCharacter::DefaultInitializer()
{
	static ConstructorHelpers::FObjectFinder<USoundBase> HitfleshAsset(
		TEXT("/Script/MetasoundEngine.MetaSoundSource'/Game/Game/Audio/Combats/MS_HitFlesh.MS_HitFlesh'")
	);
	HitfleshSound = HitfleshAsset.Object;

	static ConstructorHelpers::FObjectFinder<USoundBase> BlockingAsset(
		TEXT("/Script/MetasoundEngine.MetaSoundSource'/Game/Game/Audio/Combats/MS_Blocking.MS_Blocking'")
	);
	BlockingSound = BlockingAsset.Object;

	static ConstructorHelpers::FObjectFinder<USoundBase> KickingAsset(
		TEXT("/Script/MetasoundEngine.MetaSoundSource'/Game/Game/Audio/Combats/MS_Kicking.MS_Kicking'")
	);
	KickingSound = KickingAsset.Object;

	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> BloodSplashAsset(
		TEXT("/Script/Niagara.NiagaraSystem'/Game/Game/VFX/Combat/NS_BloodSplash.NS_BloodSplash'")
	);
	BloodSplash = BloodSplashAsset.Object;
}

// ==================== Lifecycles ==================== //

void AOWCharacter::BeginPlay()
{
	Super::BeginPlay();
	
	// Collision Events
	KickHitbox->OnComponentBeginOverlap.AddDynamic(this, &ThisClass::OnKick);
}

void AOWCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	LockOn(DeltaTime);
}

// ==================== Locomotions ==================== //

void AOWCharacter::ToggleWalk(bool bToggled)
{
	bool bTargeting = TargetCombat.IsValid();

	// When the character is still locking on enemy target, make it still using walk speed
	GetCharacterMovement()->MaxWalkSpeed = bToggled   ? WalkSpeed : 
										   bTargeting ? WalkSpeed : RunSpeed;
}

void AOWCharacter::ToggleSprint(bool bToggled)
{
	// Determine the target value
	float ToggledSpeed = TargetCombat.IsValid() ? RunSpeed  : SprintSpeed;
	float NormalSpeed  = TargetCombat.IsValid() ? WalkSpeed : RunSpeed;

	GetCharacterMovement()->MaxWalkSpeed = bToggled ? ToggledSpeed : NormalSpeed;
}

void AOWCharacter::ToggleCrouch(bool bToggled)
{
	bool bCrouched = GetCharacterMovement()->IsCrouching();

	if (bCrouched) UnCrouch();
	else			 Crouch();
}

void AOWCharacter::MoveForward()
{
	// Make sure to disable root motion first
	GetMesh()->GetAnimInstance()->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	GetCharacterMovement()->AddImpulse(GetActorForwardVector() * 400.f, true);

	// Re-enable it after certain time
	GetWorldTimerManager().SetTimer(RootMotionTimerHandler, [this]() {
		GetMesh()->GetAnimInstance()->SetRootMotionMode(ERootMotionMode::RootMotionFromMontagesOnly);
	}, .5f, false);
}

// ==================== Attributes ==================== //

void AOWCharacter::ResetState()
{
	CharacterState = ECharacterState::ECS_NoAction;
}

void AOWCharacter::Die()
{
	if (IsDead()) return;

	CharacterState = ECharacterState::ECS_Died;

	// Make sure to remove anything left
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetWorldTimerManager().ClearAllTimersForObject(this);
	GetWorldTimerManager().ClearAllTimersForObject(GetController());
	GetController()->UnPossess();
	SetActorTickEnabled(false);

	// ...
	PlayAnimMontage(Montages["Die"].LoadSynchronous());
	SetLifeSpan(5.f);

	// Enable rag doll
	FTimerHandle RagdollTimerHandle;
	GetWorldTimerManager().SetTimer(RagdollTimerHandle, [this]() {
		GetMesh()->SetCollisionProfileName(TEXT("Ragdoll"));
		GetMesh()->SetSimulatePhysics(true);

		if (CarriedWeapon.IsValid()) CarriedWeapon->Drop();
	}, 1.f, false);
}

void AOWCharacter::Stunned()
{
	EnableWeapon(false);
	PlayAnimMontage(Montages["Stunned"].LoadSynchronous());
	GetCharacterMovement()->StopMovementImmediately();

	// Un stunned after certain time
	GetWorldTimerManager().SetTimer(StunTimerHandle, this, &ThisClass::FinishedStunned, 4.5f);
}

void AOWCharacter::FinishedStunned()
{
	ResetState();
	StopAnimMontage(Montages["Stunned"].Get());
}

// ==================== Combat ==================== //

const bool AOWCharacter::IsEnemy(AOWCharacter* Other) const 
{
	if (GetTeam() == ETeam::T_Neutral || Other->GetTeam() == ETeam::T_Neutral) return false;

	return GetTeam() != Other->GetTeam();
}

void AOWCharacter::ToggleBlock(bool bToggled)
{
	if (!bEquipWeapon || !IsReady()) return;

	if (bToggled) PlayAnimMontage(Montages["Blocking"].LoadSynchronous());
	else		  StopAnimMontage(Montages["Blocking"].LoadSynchronous());

	// Reset combat
	ResetState();
	EnableWeapon(false);
}

void AOWCharacter::EnableWeapon(bool bEnabled)
{
	if (!CarriedWeapon.IsValid()) return;

	CarriedWeapon->EnableCollision(bEnabled);
}

void AOWCharacter::AttachWeapon()
{
	ResetState();

	if (!CarriedWeapon.IsValid()) return;

	CarriedWeapon->EquipTo(bEquipWeapon);
}

void AOWCharacter::SwapWeapon()
{
	if (!bAllowSwapWeapon || !IsReady() || IsOnMontage("Equipping")) return;

	// Determine which section to play the montage
	FName SectionName = bEquipWeapon ? TEXT("Equip") : TEXT("Unequip");

	// Play montage to trigger attach weapon
	PlayAnimMontage(Montages["Equipping"].LoadSynchronous(), 1.f, SectionName);
}

void AOWCharacter::SetLockOn(AOWCharacter* Target)
{
	TargetCombat = Target;

	// Adjust orient movement to false to make the locking works
	GetCharacterMovement()->bOrientRotationToMovement = !TargetCombat.IsValid();
	ToggleWalk(TargetCombat.IsValid());
}

void AOWCharacter::LockNearest()
{
	if (!bEquipWeapon) return;

	// Find nearest using sphere trace
	FVector    TraceLocation = GetActorLocation();
	FHitResult TraceResult;

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECollisionChannel::ECC_Pawn);

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.AddIgnoredActor(this);

	GetWorld()->SweepSingleByObjectType(
		TraceResult, 
		TraceLocation, 
		TraceLocation, 
		FQuat::Identity,
		ObjectParams,
		FCollisionShape::MakeSphere(650.f),
		QueryParams
	);

	if (!TraceResult.bBlockingHit)
	{
		SetLockOn(nullptr);
		
		return;
	}

	if (AOWCharacter* Other = Cast<AOWCharacter>(TraceResult.GetActor()); Other && IsEnemy(Other))
	{
		DeactivateAction();
		SetLockOn(Other);
	}
	else
		SetLockOn(nullptr);
}

void AOWCharacter::LockOn(float DeltaTime)
{
	if (!TargetCombat.IsValid() || IsOnMontage("Stunned")) return;

	FRotator CurrentRotation = GetActorRotation();
	FRotator NewRotation 	 = UKismetMathLibrary::FindLookAtRotation(GetActorLocation(), TargetCombat->GetActorLocation());
	NewRotation 		     = FMath::RInterpTo(CurrentRotation, NewRotation, DeltaTime, 5.f);

	// Only takes the new Yaw
	NewRotation.Pitch = CurrentRotation.Pitch;
	NewRotation.Roll  = CurrentRotation.Roll;

	SetActorRotation(NewRotation);

	// Lost Interest when the oponent is too far 
	float Distance = (TargetCombat->GetActorLocation() - GetActorLocation()).Size();

	if (Distance > CombatRadius || TargetCombat->IsDead())
	{
		OnLostInterest();
		SetLockOn(nullptr);
	}
}

void AOWCharacter::HitReaction(const FVector& ImpactPoint, bool bBlockable)
{
	if (IsOnMontage("Stunned")) return;

	// Get datas
	FVector CurrentLocation = GetActorLocation();
	FVector Forward         = GetActorForwardVector();
	FVector HitDirection    = (ImpactPoint - CurrentLocation).GetSafeNormal2D();

	// Depends on rad angle...its 0: Front; 1: Left; 2: Right; 3: Back
	float DotProduct = FVector::DotProduct(Forward, HitDirection);
	int32 RadAngle   = FMath::FloorToInt32(FMath::Acos(DotProduct));

	// Check if the player succeed block/avoid the hit
	bool bDodging    = IsOnMontage("Dodging");
	bSucceedBlocking = bBlockable && IsOnMontage("Blocking") && RadAngle == 0;
	bSucceedBlocking = bSucceedBlocking || bDodging;

	// Execute when the character is not dodging so the dodging animation montage won't be interupted
	if (!bDodging)
	{
		// Animation Montage (Depends on succeed blocking or no)
		FName MontageToPlay  = bSucceedBlocking ? TEXT("Blocking") : TEXT("Hit React");
		FName MontageSection = bSucceedBlocking ? TEXT("Blocking") : *FString::Printf(TEXT("From%d"), RadAngle);
		PlayAnimMontage(Montages[MontageToPlay].LoadSynchronous(), 1.f, MontageSection);

		// Knock back
		float KnockbackPower = bSucceedBlocking ? 200.f : 400.f;
		GetCharacterMovement()->AddImpulse(-HitDirection * KnockbackPower, true);
	}
}

void AOWCharacter::ComboOver()
{
	AttackCount = 0;
}

void AOWCharacter::StartKick()
{
	KickHitbox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

	CharacterState = ECharacterState::ECS_Action;
	PlayAnimMontage(Montages["Attacking"].LoadSynchronous(), 1.f, TEXT("Kicking"));
}

void AOWCharacter::OnKick(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (OtherActor == this) return;

	IHitInterface* ActorHit = Cast<IHitInterface>(OtherActor);
	
	if (!ActorHit || !ActorHit->IsEnemy(this)) return;

	// Just make other actor hit to be unblocked
	ActorHit->OnWeaponHit(this, KickHitbox->GetComponentLocation(), 0.f, false);

	// Don't forget to disable the collision :)
	KickHitbox->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Play audio
	UGameplayStatics::PlaySoundAtLocation(
		this,
		KickingSound.LoadSynchronous(),
		KickHitbox->GetComponentLocation()
	);
}

void AOWCharacter::Attack()
{
	if (!bEquipWeapon || !IsReady()) return;

	CharacterState = ECharacterState::ECS_Action;
	GetCharacterMovement()->StopMovementImmediately();

    // Make sure to make noise since the attacking has whoosh sound
    MakeNoise(1.f, this, GetActorLocation(), 500.f);

	// Which attack will be?
	if (bCharging) ChargeAttack();
	else		   AttackCombo();

    // Reset
	DamageMultiplier = 1.f;
	bCharging        = false;
	GetWorldTimerManager().ClearTimer(ChargeTimerHandle);
}

void AOWCharacter::AttackCombo()
{
    // Play montage depends on the carried weapon
    FName AttackCombo = *FString::Printf(TEXT("%s%d"), *CarriedWeapon->GetWeaponName(), AttackCount);
    UAnimMontage *Montage = Montages["Attacking"].LoadSynchronous();
    PlayAnimMontage(Montage, 1.f, AttackCombo);

    // Updating combo, don't forget to update the combo over too
    AttackCount = (AttackCount + 1) % 3;
    GetWorldTimerManager().SetTimer(
        ComboOverHandler,
        this,
        &ThisClass::ComboOver,
        ComboOverTimer
	);
}

void AOWCharacter::StartChargeAttack()
{
	// If already attacking/on charge attack already
	if (!bEquipWeapon || IsOnMontage("Attacking")) return;

	DamageMultiplier += DamageMultiplierRate * GetWorld()->GetDeltaSeconds();

	// Start timer for the first time
	if (!GetWorldTimerManager().IsTimerActive(ChargeTimerHandle) && !bCharging)
	{
		GetWorldTimerManager().ClearTimer(ChargeTimerHandle);
		GetWorldTimerManager().SetTimer(ChargeTimerHandle, this, &ThisClass::OnChargeAttack, ChargeAfter);
	}
}

void AOWCharacter::OnChargeAttack()
{
	// ...
	bCharging = true;

	LockNearest();
	PlayAnimMontage(Montages["Charge Attack"].LoadSynchronous());

	// Start timer to perform actual charge attack
	GetWorldTimerManager().ClearTimer(ChargeTimerHandle);
	GetWorldTimerManager().SetTimer(ChargeTimerHandle, this, &ThisClass::Attack, AutoChargeTimer);
}

void AOWCharacter::ChargeAttack()
{
    FName ChargeAttackSection = *FString::Printf(TEXT("%sChargeAttack"), *CarriedWeapon->GetWeaponName());
    PlayAnimMontage(Montages["Attacking"].LoadSynchronous(), 1.f, ChargeAttackSection);

    CarriedWeapon->SetTempDamage(
        CarriedWeapon->GetDamage() * DamageMultiplier,
		false
	);
}

void AOWCharacter::OnWeaponHit(AOWCharacter* DamagingCharacter, const FVector& ImpactPoint, const float GivenDamage, bool bBlockable)
{
	if (IsDead()) return;

	// Hit React
	HitReaction(ImpactPoint, bBlockable);

	// Reset combat
	ResetState();
	EnableWeapon(false);

	if (!bSucceedBlocking)
	{
		ToggleBlock(false);

		if (GivenDamage > 0.f)
		{
			// Show hit visualization
			UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, BloodSplash.LoadSynchronous(), ImpactPoint);

			// Reduce health
			SetHealth(-GivenDamage);
		}
	}

	// Sound (Blocking or hitflesh sound)
	if (GivenDamage > 0.f)
		UGameplayStatics::PlaySoundAtLocation(
			this, 
			bSucceedBlocking ? BlockingSound.LoadSynchronous() : HitfleshSound.LoadSynchronous(), 
			ImpactPoint
		);
}

// ==================== Audio ==================== //

void AOWCharacter::PlayFootstepSound()
{
	bool bCrouching 		= GetCharacterMovement()->IsCrouching();
	FVector CurrentLocation = GetActorLocation();

	// Only make noises when the player is not crouching ofc
	if (!bCrouching)
		MakeNoise(1.f, this, CurrentLocation, 500.f);

	if (FootstepSounds.Contains(TEXT("Concrete")))
		UGameplayStatics::PlaySoundAtLocation(
			this, 
			FootstepSounds["Concrete"].LoadSynchronous(), 
			CurrentLocation,
			bCrouching ? .4f : 1.f,
			bCrouching ? .8f : 1.f
		);
}
