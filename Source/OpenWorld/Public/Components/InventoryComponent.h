// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InventoryComponent.generated.h"

class AMeleeWeapon;
class APlayerCharacter;

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class OPENWORLD_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	UInventoryComponent();

	// ===== Weaponary ========== //

	void AddWeapon(AMeleeWeapon* NewWeapon);
	void ChangeWeapon(int8 Slot);

#if WITH_EDITOR
	FORCEINLINE void RefreshSlot()
	{
		Weapons.SetNum(WeaponMaxSlot);
	}
#endif

protected:
	// ===== Lifecycles ========== //

	virtual void BeginPlay() override;

private:
	// ===== Attributes ========== //

	UPROPERTY()
	TWeakObjectPtr<APlayerCharacter> PlayerCharacter;

	// ===== Weaponary ========== //

	UPROPERTY()
	TArray<TWeakObjectPtr<AMeleeWeapon>> Weapons;

	int8 CurrentWeapon = 0;
	int8 LastWeapon    = CurrentWeapon;
	
	UPROPERTY(EditAnywhere, Category=Weaponary)
	int8 WeaponMaxSlot = 2;

	// *** Unequip Weapon *** //

	FTimerHandle UnequipingTimerHandle;
	FORCEINLINE void UnequipWeapon();

	int8 FindEmptySlot();
};
